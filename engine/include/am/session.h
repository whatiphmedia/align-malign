// Auto Malign session bus: links every plugin instance in the host process.
//
// Each instance (one per track) owns a Slot. While "Learn" is on and the transport plays, the audio thread
// writes its mono mid signal into a lock-free ring indexed by *host timeline sample*, so every track's audio
// lines up by song position no matter which thread or order the host processes tracks in. A single worker
// thread picks up timeline frames that every linked track has written, accumulates cross-spectra, solves,
// and hands each slot its impulse responses.
//
// Audio-thread API (lock-free, no allocation): Slot::write().
// Everything else runs on the message thread or the worker thread.
#pragma once
#include "engine.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace am {

constexpr int kRingBits = 20;                    // 1,048,576 samples (~21.8 s at 48 kHz)
constexpr int64_t kRing = int64_t(1) << kRingBits;
constexpr int kChunks = int(kRing / kHop);       // 256 chunks of 4096 samples

struct IRSet {                                   // what a slot convolves with
    std::vector<float> mid, side;                // main path (kIRLen taps each)
    std::vector<float> midMirror, sideMirror;    // knob mirrored (used by Motion)
    uint64_t version = 0;
};

struct SlotControls {                            // written from the message thread, read by the worker
    std::atomic<float> knob{0.f};                // -1 align .. +1 malign
    std::atomic<int> flavour{0};
    std::atomic<float> match{1.f};               // 0..1
    std::atomic<bool> motionOn{false};
};

class SessionBus;

class Slot {
public:
    explicit Slot(int id) : id_(id), ring_((size_t)kRing, 0.f), stamp_(kChunks) {
        for (auto& s : stamp_) s.store(-1, std::memory_order_relaxed);
    }

    int id() const { return id_; }
    SlotControls controls;

    // ---- audio thread ------------------------------------------------------
    // mono: mid signal for this block. pos: host timeline position of the first sample (samples).
    void write(const float* mono, int n, int64_t pos, bool playing, bool learning) noexcept {
        if (!playing || !learning || pos < 0) { expectNext_ = -1; return; }
        if (pos != expectNext_) validFrom_ = pos;   // transport jump / loop: restart contiguity
        expectNext_ = pos + n;
        for (int i = 0; i < n; ++i) {
            const int64_t p = pos + i;
            ring_[(size_t)(p & (kRing - 1))] = mono[i];
            if (((p + 1) % kHop) == 0) {
                const int64_t chunk = (p + 1) / kHop - 1;
                if (validFrom_ <= chunk * kHop) {
                    stamp_[(size_t)(chunk % kChunks)].store(chunk, std::memory_order_release);
                    latest_.store(chunk, std::memory_order_release);
                }
            }
        }
    }

    // ---- worker thread -----------------------------------------------------
    bool hasChunk(int64_t c) const { return c >= 0 && stamp_[(size_t)(c % kChunks)].load(std::memory_order_acquire) == c; }
    int64_t latest() const { return latest_.load(std::memory_order_acquire); }
    void readFrame(int64_t lastChunk, float* dst) const { // kSolveN samples ending at the end of lastChunk
        const int64_t start = (lastChunk + 1) * kHop - kSolveN;
        for (int i = 0; i < kSolveN; ++i) dst[i] = ring_[(size_t)((start + i) & (kRing - 1))];
    }

    // results (guarded by bus mutex)
    std::string name;
    Maps ownMaps;            // this track's solved maps (index 0 of a 1-track Maps), persisted in plugin state
    std::vector<float> matchDb;
    std::function<void(std::shared_ptr<const IRSet>)> onIR; // set by the plugin; called on the worker thread

private:
    friend class SessionBus;
    int id_;
    std::vector<float> ring_;
    std::vector<std::atomic<int64_t>> stamp_;
    std::atomic<int64_t> latest_{-1};
    int64_t expectNext_ = -1, validFrom_ = 0;  // audio-thread only
};

struct SessionStatus {
    int linked = 0;
    bool learning = false;
    int64_t frames = 0;
    double seconds = 0;
    bool solved = false;
    int anchor = -1;
};

class SessionBus {
public:
    static SessionBus& get() {
        static SessionBus bus;
        return bus;
    }

    std::shared_ptr<Slot> join(double sampleRate) {
        std::lock_guard<std::mutex> g(m_);
        if (!worker_.joinable()) { quit_ = false; worker_ = std::thread([this] { run(); }); }
        auto s = std::make_shared<Slot>(nextId_++);
        slots_.push_back(s);
        if (sampleRate > 0) sr_ = sampleRate;
        restartAccumulationLocked();
        dirty_ = true;
        cv_.notify_all();
        return s;
    }

    // The worker thread lives only while at least one instance is linked. It is joined here, from the last
    // instance's destructor, never from a static destructor (joining a thread during DLL unload deadlocks on Windows).
    void leave(const std::shared_ptr<Slot>& s) {
        std::thread finished;
        {
            std::lock_guard<std::mutex> g(m_);
            slots_.erase(std::remove(slots_.begin(), slots_.end(), s), slots_.end());
            s->onIR = nullptr;
            restartAccumulationLocked();
            if (slots_.empty() && worker_.joinable()) {
                quit_ = true;
                learning_ = false;
                cv_.notify_all();
                finished = std::move(worker_);
            }
        }
        if (finished.joinable()) finished.join();
    }

    void setSampleRate(double sr) {
        std::lock_guard<std::mutex> g(m_);
        if (sr > 0 && sr != sr_) { sr_ = sr; restartAccumulationLocked(); dirty_ = true; cv_.notify_all(); }
    }

    // Learn toggles session-wide: every linked instance captures while the song plays.
    void setLearning(bool on) {
        std::lock_guard<std::mutex> g(m_);
        if (on && !learning_) restartAccumulationLocked();
        if (!on && learning_) solveRequested_ = true;
        learning_.store(on);
        cv_.notify_all();
    }
    bool learning() const { return learning_.load(); }

    void controlsChanged() { std::lock_guard<std::mutex> g(m_); dirty_ = true; cv_.notify_all(); }
    void requestSolve() { std::lock_guard<std::mutex> g(m_); solveRequested_ = true; cv_.notify_all(); }

    SessionStatus status() const {
        std::lock_guard<std::mutex> g(m_);
        SessionStatus st;
        st.linked = (int)slots_.size();
        st.learning = learning_.load();
        st.frames = cs_.frames;
        st.seconds = cs_.frames * (double)kHop / sr_;
        st.solved = solvedOnce_;
        st.anchor = anchor_;
        return st;
    }

    std::vector<std::string> trackNames() const {
        std::lock_guard<std::mutex> g(m_);
        std::vector<std::string> v;
        for (auto& s : slots_) v.push_back(s->name);
        return v;
    }

    void setName(const std::shared_ptr<Slot>& s, std::string name) {
        std::lock_guard<std::mutex> g(m_);
        s->name = std::move(name);
    }

    // Copy of a slot's solved maps and match curve (for saving plugin state).
    void snapshot(const std::shared_ptr<Slot>& s, Maps& maps, std::vector<float>& matchDb) const {
        std::lock_guard<std::mutex> g(m_);
        maps = s->ownMaps;
        matchDb = s->matchDb;
    }

    // Restoring a saved instance: give it back its maps without a new learn pass.
    void restore(const std::shared_ptr<Slot>& s, Maps maps, std::vector<float> matchDb) {
        std::lock_guard<std::mutex> g(m_);
        s->ownMaps = std::move(maps);
        s->matchDb = std::move(matchDb);
        dirty_ = true;
        cv_.notify_all();
    }

    ~SessionBus() {
        // Normally already stopped by the last leave(). If not, don't join during process/DLL teardown.
        { std::lock_guard<std::mutex> g(m_); quit_ = true; cv_.notify_all(); }
        if (worker_.joinable()) worker_.detach();
    }

private:
    SessionBus() : fft_(kSolveN), win_(hann(kSolveN)) {}

    void restartAccumulationLocked() {
        order_ = slots_;
        cs_.reset((int)order_.size(), sr_);
        frameDone_.assign(kChunks, -1);
    }

    void run() {
        std::vector<float> frame(kSolveN);
        std::vector<cplx> scratch;
        std::vector<std::vector<cplx>> bins;
        auto lastSolve = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(m_);
        while (!quit_) {
            cv_.wait_for(lk, std::chrono::milliseconds(40));
            if (quit_) break;

            // 1) accumulate any timeline frames every linked track has written
            if (learning_ && order_.size() >= 2) {
                int64_t newest = -1;
                for (auto& s : order_) newest = std::max(newest, s->latest());
                const int K = cs_.K;
                bins.resize(order_.size());
                for (int64_t c = std::max<int64_t>(3, newest - (kChunks - 16)); c <= newest; ++c) {
                    if (frameDone_[(size_t)(c % kChunks)] == c) continue;
                    bool ok = true;
                    for (auto& s : order_)
                        for (int64_t q = c - 3; q <= c && ok; ++q) ok = s->hasChunk(q);
                    if (!ok) continue;
                    std::vector<const cplx*> ptrs;
                    for (size_t t = 0; t < order_.size(); ++t) {
                        order_[t]->readFrame(c, frame.data());
                        bins[t].resize(K);
                        analyseFrame(fft_, win_, frame.data(), scratch, bins[t].data(), K);
                        ptrs.push_back(bins[t].data());
                    }
                    bool stillValid = true; // the audio thread may have lapped the ring while we read
                    for (auto& s : order_) stillValid = stillValid && s->hasChunk(c - 3);
                    if (stillValid) cs_.addFrame(ptrs);
                    frameDone_[(size_t)(c % kChunks)] = c;
                }
                const auto now = std::chrono::steady_clock::now();
                if (cs_.frames > 8 && now - lastSolve > std::chrono::milliseconds(1500)) { solveRequested_ = true; lastSolve = now; }
            }

            // 2) solve when asked (and periodically while learning, for live feedback)
            if (solveRequested_) {
                solveRequested_ = false;
                if (cs_.frames > 0 && cs_.T >= 2) { solveLocked(); dirty_ = true; }
            }

            // 3) rebuild impulse responses when maps or controls changed
            if (dirty_) { dirty_ = false; rebuildLocked(); }
        }
    }

    void solveLocked() {
        anchor_ = autoAnchor(cs_);
        auto aS = solve(cs_, anchor_, alignStandard());
        auto mS = solve(cs_, anchor_, malignStandard());
        auto aE = solve(cs_, anchor_, alignExtended());
        auto mE = solve(cs_, anchor_, malignExtended());
        for (size_t t = 0; t < order_.size(); ++t) {
            Maps m;
            m.binHz = cs_.binHz; m.K = cs_.K;
            m.alignStd = {aS[t]}; m.malignStd = {mS[t]}; m.alignExt = {aE[t]}; m.malignExt = {mE[t]};
            order_[t]->ownMaps = std::move(m);
        }
        solvedOnce_ = true;
    }

    void rebuildLocked() {
        const double binHz = sr_ / kSolveN;
        const int K = std::min(kSolveN / 2, (int)std::ceil(kMaxSolveHz / binHz) + 2);
        // effective mid curves for every slot (needed for the shared match EQ)
        std::vector<PhaseCurves> curves(slots_.size());
        std::vector<std::vector<float>> mids;
        for (size_t t = 0; t < slots_.size(); ++t) {
            auto& s = slots_[t];
            Maps m = s->ownMaps;
            if (m.K != K) { // no solve yet (or a different sample rate): neutral maps of the right size
                m.K = K; m.binHz = binHz;
                m.alignStd = m.malignStd = m.alignExt = m.malignExt = {std::vector<float>(K, 0.f)};
            }
            Controls c; c.knob = s->controls.knob.load(); c.flavour = (Flavour)s->controls.flavour.load(); c.seed = (uint32_t)s->id() + 1;
            curves[t] = effectivePhase(m, 0, c);
            mids.push_back(curves[t].mid);
        }
        // Match EQ: only meaningful when the slots are the ones the cross-spectra were measured on.
        const bool sameSet = (order_ == slots_) && cs_.frames > 0 && cs_.K == K;
        std::vector<float> match = sameSet ? matchGainDb(cs_, mids) : std::vector<float>();
        for (size_t t = 0; t < slots_.size(); ++t) {
            auto& s = slots_[t];
            if (sameSet) s->matchDb = match;
            const std::vector<float>& g = s->matchDb.size() == (size_t)K ? s->matchDb : std::vector<float>();
            const double amt = s->controls.match.load();
            auto set = std::make_shared<IRSet>();
            set->mid = buildIR(curves[t].mid, g, binHz, sr_, amt);
            set->side = buildIR(curves[t].side, g, binHz, sr_, amt);
            if (s->controls.motionOn.load()) {
                Maps m = s->ownMaps;
                if (m.K != K) { m.K = K; m.binHz = binHz; m.alignStd = m.malignStd = m.alignExt = m.malignExt = {std::vector<float>(K, 0.f)}; }
                Controls c; c.knob = -s->controls.knob.load(); c.flavour = (Flavour)s->controls.flavour.load(); c.seed = (uint32_t)s->id() + 1;
                const PhaseCurves mc = effectivePhase(m, 0, c);
                set->midMirror = buildIR(mc.mid, g, binHz, sr_, amt);
                set->sideMirror = buildIR(mc.side, g, binHz, sr_, amt);
            }
            set->version = ++irVersion_;
            if (s->onIR) s->onIR(set);
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    bool quit_ = false, dirty_ = false, solveRequested_ = false, solvedOnce_ = false;
    std::atomic<bool> learning_{false};
    std::vector<std::shared_ptr<Slot>> slots_, order_;
    std::vector<int64_t> frameDone_ = std::vector<int64_t>(kChunks, -1);
    CrossSpectra cs_;
    FFT fft_;
    std::vector<double> win_;
    double sr_ = 48000;
    int nextId_ = 0, anchor_ = -1;
    uint64_t irVersion_ = 0;
};

} // namespace am
