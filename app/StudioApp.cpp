// Auto Malign Studio: standalone stem session. Load a folder of stems, learn them, align / malign with native effects,
// audition (processed / original / mono), export. Undo/redo, debug log. The DSP lives in engine/include/am/studio.
#include <JuceHeader.h>
#include "BinaryData.h"
#include "am/studio/studio.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

using namespace am::studio;

namespace {
std::vector<std::byte> toBytes(const char* d, int n) { std::vector<std::byte> v((size_t)n); std::memcpy(v.data(), d, (size_t)n); return v; }
juce::File appDataDir() { auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("AutoMalignStudio"); d.createDirectory(); return d; }
std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }
} // namespace

// ------------------------------------------------------------------------------------------------ playback
class Player : public juce::AudioSource {
public:
    struct Buffers { std::shared_ptr<const Stereo> processed, original; int sr = 44100; };
    void setBuffers(Buffers b) { std::lock_guard<std::mutex> g(m_); buf_ = std::move(b); }
    void prepareToPlay(int, double sampleRate) override { deviceSr_ = sampleRate; }
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        info.clearActiveBufferRegion();
        Buffers b; { std::unique_lock<std::mutex> g(m_, std::try_to_lock); if (!g.owns_lock()) return; b = buf_; }
        const auto& src = monitor.load() == 1 ? b.original : b.processed;
        if (!playing.load() || !src || src->size() == 0) return;
        const double step = b.sr / deviceSr_; double pos = position.load();
        auto* L = info.buffer->getWritePointer(0, info.startSample); auto* R = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer(1, info.startSample) : nullptr;
        const bool mono = monoCheck.load();
        for (int i = 0; i < info.numSamples; ++i) {
            if (pos >= (double)src->size() - 1) { pos = loopStart.load(); if (!loop.load()) { playing = false; break; } }
            const float l = sampleAt(src->L, pos), r = sampleAt(src->R, pos);
            if (mono) { const float m = 0.5f * (l + r); L[i] = m; if (R) R[i] = m; } else { L[i] = l; if (R) R[i] = r; }
            peakL = std::max(peakL.load(), std::abs(L[i])); if (R) peakR = std::max(peakR.load(), std::abs(R[i]));
            pos += step;
        }
        position = pos;
    }
    std::atomic<double> position{ 0 }, loopStart{ 0 };
    std::atomic<bool> playing{ false }, monoCheck{ false }, loop{ false };
    std::atomic<int> monitor{ 0 };   // 0 processed, 1 original
    std::atomic<float> peakL{ 0 }, peakR{ 0 };
private:
    std::mutex m_; Buffers buf_; double deviceSr_ = 44100;
};

// ------------------------------------------------------------------------------------------------ background jobs
class Worker : public juce::Thread {
public:
    Worker() : juce::Thread("Auto Malign Studio worker") { startThread(); }
    ~Worker() override { signalThreadShouldExit(); notify(); stopThread(10000); }
    void post(std::string label, std::function<void()> f) { { std::lock_guard<std::mutex> g(m_); q_.push_back({ std::move(label), std::move(f) }); } notify(); }
    bool busy() const { return busy_.load(); }
    std::string current() const { std::lock_guard<std::mutex> g(m_); return cur_; }
    size_t queued() const { std::lock_guard<std::mutex> g(m_); return q_.size(); }
    void run() override {
        while (!threadShouldExit()) {
            std::pair<std::string, std::function<void()>> job;
            { std::lock_guard<std::mutex> g(m_); if (!q_.empty()) { job = std::move(q_.front()); q_.pop_front(); cur_ = job.first; } }
            if (!job.second) { wait(200); continue; }
            busy_ = true;
            try { job.second(); } catch (const std::exception& e) { AM_LOG(Error) << job.first << " failed: " << e.what(); }
            busy_ = false; { std::lock_guard<std::mutex> g(m_); cur_.clear(); }
        }
    }
private:
    mutable std::mutex m_; std::deque<std::pair<std::string, std::function<void()>>> q_; std::string cur_; std::atomic<bool> busy_{ false };
};

// ------------------------------------------------------------------------------------------------ main component
class StudioComponent : public juce::Component, private juce::Timer {
public:
    StudioComponent() {
        Log::get().setFile(appDataDir().getChildFile("studio.log").getFullPathName().toStdString());
        AM_LOG(Info) << "Auto Malign Studio started; log file " << appDataDir().getChildFile("studio.log").getFullPathName().toStdString();
        auto opts = juce::WebBrowserComponent::Options{}
            .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
            .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}
                                        .withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("AutoMalignStudioWebView2"))
                                        .withBackgroundColour(juce::Colour(0xff050505)))
            .withNativeIntegrationEnabled()
            .withResourceProvider([](const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource> {
                const auto path = url.fromFirstOccurrenceOf(juce::WebBrowserComponent::getResourceProviderRoot(), false, false).upToFirstOccurrenceOf("?", false, false);
                if (path.isEmpty() || path == "/" || path.endsWith("index.html")) return juce::WebBrowserComponent::Resource{ toBytes(BinaryData::studio_html, BinaryData::studio_htmlSize), "text/html" };
                return std::nullopt; })
            .withEventListener("action", [this](juce::var v) { onAction(v); });
        web_ = std::make_unique<juce::WebBrowserComponent>(opts);
        addAndMakeVisible(*web_); web_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
        devices_.initialiseWithDefaultDevices(0, 2); source_.setSource(&player_); devices_.addAudioCallback(&source_);
        setSize(1440, 860); startTimerHz(15);
    }
    ~StudioComponent() override { stopTimer(); devices_.removeAudioCallback(&source_); source_.setSource(nullptr); worker_.reset(); }
    void resized() override { if (web_) web_->setBounds(getLocalBounds()); }

private:
    // -------------------------------------------------------------------------------------- UI actions
    void onAction(const juce::var& v) {
        const auto name = v["name"].toString();
        auto num = [&](const char* k, double d = 0) { return v.hasProperty(k) ? (double)v[k] : d; };
        if (name == "ready") { stateVersion_++; overviewSent_ = false; }
        else if (name == "openFolder") chooseFolder();
        else if (name == "openSession") chooseSession();
        else if (name == "save") saveSession();
        else if (name == "export") chooseExport();
        else if (name == "play") { player_.playing = !player_.playing.load(); }
        else if (name == "stop") { player_.playing = false; player_.position = 0; }
        else if (name == "seek") player_.position = std::max(0.0, num("t") * studio_.sr);
        else if (name == "loop") player_.loop = num("on") > 0.5;
        else if (name == "monitor") player_.monitor = (int)num("v");
        else if (name == "mono") player_.monoCheck = num("on") > 0.5;
        else if (name == "param") { { std::lock_guard<std::mutex> g(stateMx_); studio_.set(v["id"].toString().toStdString(), num("v"), true); } changed(); }
        else if (name == "endDrag") { std::lock_guard<std::mutex> g(stateMx_); studio_.history.breakMerge(); }
        else if (name == "track") {
            const int i = (int)num("i"); const auto field = v["field"].toString().toStdString();
            { std::lock_guard<std::mutex> g(stateMx_); studio_.setTrack(i, field, num("v")); }
            if (field == "role" || field == "ref") worker_->post("re-learn the vocal stack", [this] { studio_.relearnVocals(); });
            changed();
        }
        else if (name == "preset") { { std::lock_guard<std::mutex> g(stateMx_); studio_.applyPresetNamed(v["preset"].toString().toStdString()); } changed(); }
        else if (name == "undo") { { std::lock_guard<std::mutex> g(stateMx_); studio_.undo(); } changed(); }
        else if (name == "redo") { { std::lock_guard<std::mutex> g(stateMx_); studio_.redo(); } changed(); }
        else if (name == "relearn") {
            const am::studio::Range r{ (size_t)std::max(0.0, num("a") * studio_.sr), (size_t)std::max(0.0, num("b") * studio_.sr) }; const int track = (int)num("track", -1);
            worker_->post("re-learn section", [this, r, track] { studio_.relearnSection(r, track); }); changed();
        }
        else if (name == "logLevel") Log::get().setLevel((Log::Level)(int)num("v"));
        else if (name == "clearLog") Log::get().clear();
        else if (name == "copyLog") juce::SystemClipboard::copyTextToClipboard(Log::get().all());
        else if (name == "openLogFolder") appDataDir().revealToUser();
    }
    void changed() { stateVersion_++; dirty_ = true; lastChange_ = juce::Time::getMillisecondCounterHiRes(); }

    // -------------------------------------------------------------------------------------- files
    void chooseFolder() {
        chooser_ = std::make_unique<juce::FileChooser>("Choose a folder of stems (WAV)", juce::File(), "*", true);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
            const auto dir = fc.getResult(); if (!dir.isDirectory()) return;
            player_.playing = false;
            worker_->post("load and learn", [this, d = dir.getFullPathName().toStdString()] {
                Studio fresh; fresh.loadFolder(d); fresh.learnAll();
                { std::lock_guard<std::mutex> g(stateMx_); studio_ = std::move(fresh); sessionFile_.clear(); }
                overviewSent_ = false; changed();
            });
        });
    }
    void chooseSession() {
        chooser_ = std::make_unique<juce::FileChooser>("Open a session", juce::File(), "*.amsession", true);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            const auto f = fc.getResult(); if (!f.existsAsFile()) return;
            player_.playing = false;
            worker_->post("open session", [this, p = f.getFullPathName().toStdString()] {
                Studio fresh; if (!fresh.loadProject(p)) return;
                { std::lock_guard<std::mutex> g(stateMx_); studio_ = std::move(fresh); sessionFile_ = p; }
                overviewSent_ = false; changed();
            });
        });
    }
    void saveSession() {
        chooser_ = std::make_unique<juce::FileChooser>("Save the session", sessionFile_.empty() ? juce::File() : juce::File(sessionFile_), "*.amsession", true);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult(); if (f == juce::File()) return; if (!f.hasFileExtension("amsession")) f = f.withFileExtension("amsession");
            std::lock_guard<std::mutex> g(stateMx_); if (studio_.saveProject(f.getFullPathName().toStdString())) sessionFile_ = f.getFullPathName().toStdString();
        });
    }
    void chooseExport() {
        chooser_ = std::make_unique<juce::FileChooser>("Export processed stems, effect layers and the mix to a folder", juce::File(), "*", true);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
            const auto dir = fc.getResult(); if (dir == juce::File()) return;
            worker_->post("export", [this, d = dir.getFullPathName().toStdString()] { ProjectState snap; { std::lock_guard<std::mutex> g(stateMx_); snap = studio_.state; } snap.bypass = 0; studio_.exportAll(d, studio_.render(snap)); });
        });
    }

    // -------------------------------------------------------------------------------------- render + UI updates
    void timerCallback() override {
        // re-render a moment after the last change (dragging a knob renders once you pause)
        if (dirty_ && !worker_->busy() && worker_->queued() == 0 && juce::Time::getMillisecondCounterHiRes() - lastChange_ > 250 && studio_.length() > 0) {
            dirty_ = false;
            worker_->post("render", [this] {
                ProjectState snap; { std::lock_guard<std::mutex> g(stateMx_); snap = studio_.state; } snap.bypass = 0;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                auto R = studio_.render(snap);
                Player::Buffers b; b.sr = studio_.sr; b.processed = std::make_shared<Stereo>(std::move(R.mix)); b.original = std::make_shared<Stereo>(std::move(R.original));
                lastPeak_ = std::max(b.processed->peak(), 1e-9f); player_.setBuffers(std::move(b));
                AM_LOG(Info) << "render " << (int)(juce::Time::getMillisecondCounterHiRes() - t0) << " ms";
            });
        }
        if (!web_) return;
        // full state when it changed, otherwise only the light transport/meters/log update
        std::string msg = "{\"t\":" + num(player_.position.load() / std::max(1, studio_.sr)) + ",\"playing\":" + (player_.playing.load() ? "true" : "false")
            + ",\"busy\":" + (worker_->busy() || worker_->queued() || dirty_ ? "true" : "false") + ",\"job\":" + q(worker_->current())
            + ",\"peakL\":" + num(player_.peakL.exchange(0)) + ",\"peakR\":" + num(player_.peakR.exchange(0)) + ",\"renderPeak\":" + num(lastPeak_.load());
        std::vector<std::string> lines; logSerial_ = Log::get().since(logSerial_, lines);
        msg += ",\"log\":["; for (size_t i = 0; i < lines.size(); ++i) msg += (i ? "," : "") + q(lines[i]); msg += "]";
        if (stateVersion_ != sentVersion_) { sentVersion_ = stateVersion_.load(); std::lock_guard<std::mutex> g(stateMx_); msg += ",\"session\":" + sessionJson(); }
        if (!overviewSent_ && studio_.length() > 0 && !worker_->busy()) { overviewSent_ = true; std::lock_guard<std::mutex> g(stateMx_); msg += ",\"overview\":" + overviewJson(); }
        msg += "}";
        web_->emitEventIfBrowserIsVisible("studio", juce::var(juce::String(msg)));
    }
    std::string sessionJson() {
        auto& s = studio_; std::string o = "{\"length\":" + num(s.length() / (double)std::max(1, s.sr)) + ",\"sr\":" + num(s.sr) + ",\"bpm\":" + num(s.bpm()) + ",\"key\":" + q(s.key().name())
            + ",\"file\":" + q(sessionFile_) + ",\"canUndo\":" + (s.history.canUndo() ? "true" : "false") + ",\"canRedo\":" + (s.history.canRedo() ? "true" : "false")
            + ",\"undoLabel\":" + q(s.history.undoLabel()) + ",\"redoLabel\":" + q(s.history.redoLabel()) + ",\"state\":" + toJson(s.state);
        o += ",\"paramDefs\":["; bool first = true;
        for (auto& p : params()) {
            o += std::string(first ? "" : ",") + "{\"id\":" + q(p.id) + ",\"label\":" + q(p.label) + ",\"group\":" + q(p.group) + ",\"min\":" + num(p.min) + ",\"max\":" + num(p.max) + ",\"def\":" + num(p.def)
               + ",\"switch\":" + (p.isSwitch ? "true" : "false") + ",\"choices\":["; for (size_t c = 0; c < p.choices.size(); ++c) o += (c ? "," : "") + q(p.choices[c]); o += "]}"; first = false;
        }
        o += "],\"presets\":["; first = true;
        for (auto& p : presets()) { o += std::string(first ? "" : ",") + "{\"name\":" + q(p.name) + ",\"family\":" + q(p.family) + ",\"mode\":" + q(p.mode) + ",\"info\":" + q(p.info) + "}"; first = false; }
        o += "],\"gaps\":[";   // the learned vocal gaps (only while the worker is idle: it owns the learned data)
        if (!worker_->busy()) for (size_t i = 0; i < s.vocalsLearned.gaps.size(); ++i) o += std::string(i ? "," : "") + "[" + num(s.vocalsLearned.gaps[i].a / (double)s.sr) + "," + num(s.vocalsLearned.gaps[i].b / (double)s.sr) + "]";
        return o + "]}";
    }
    // Per track, 800 columns of [peak, low, mid, high]: the Serato / KracKill$ Vision colouring (red = lows under 200 Hz,
    // green = mids 200 Hz-2 kHz, blue = highs above 2 kHz; the UI normalises them into a colour per column).
    std::string overviewJson() {
        std::string o = "["; const int P = 800; const int sr = studio_.sr;
        for (size_t t = 0; t < studio_.audio.size(); ++t) {
            const auto& a = studio_.audio[t]; const auto m = a.mono();
            auto lo = m, hi = m, mid = m; lowpass(lo, 200, sr); lowpass(lo, 200, sr); highpass(hi, 2000, sr); highpass(hi, 2000, sr); bandpass(mid, 200, 2000, sr);
            o += (t ? ",[" : "["); const size_t step = std::max<size_t>(1, m.size() / P);
            for (int p = 0; p < P; ++p) {
                float pk = 0; double el = 0, em = 0, eh = 0; const size_t i0 = p * step, i1 = std::min(m.size(), (p + 1) * step);
                for (size_t i = i0; i < i1; i += 2) { pk = std::max({ pk, std::abs(a.L[i]), std::abs(a.R[i]) }); el += lo[i] * lo[i]; em += mid[i] * mid[i]; eh += hi[i] * hi[i]; }
                const double n = std::max<double>(1, (i1 - i0) / 2.0);
                o += std::string(p ? "," : "") + "[" + num(std::round(pk * 1000) / 1000) + "," + num(std::round(std::sqrt(el / n) * 1000) / 1000) + "," + num(std::round(std::sqrt(em / n) * 1000) / 1000) + "," + num(std::round(std::sqrt(eh / n) * 1000) / 1000) + "]";
            }
            o += "]";
        }
        return o + "]";
    }

    std::unique_ptr<juce::WebBrowserComponent> web_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::AudioDeviceManager devices_; juce::AudioSourcePlayer source_; Player player_;
    Studio studio_; std::mutex stateMx_; std::string sessionFile_;
    std::unique_ptr<Worker> worker_ = std::make_unique<Worker>();
    std::atomic<long> stateVersion_{ 1 }; long sentVersion_ = 0; long logSerial_ = 0; std::atomic<bool> overviewSent_{ false };
    std::atomic<bool> dirty_{ false }; double lastChange_ = 0; std::atomic<float> lastPeak_{ 0 };
};

// ------------------------------------------------------------------------------------------------ app
class StudioApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Auto Malign Studio"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override { window_ = std::make_unique<Window>(); }
    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    struct Window : juce::DocumentWindow {
        Window() : juce::DocumentWindow("Auto Malign Studio  |  KracKill$", juce::Colour(0xff050505), juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true); setContentOwned(new StudioComponent(), true); setResizable(true, true); setResizeLimits(960, 600, 4000, 2400);
            centreWithSize(getWidth(), getHeight()); setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window_;
};

START_JUCE_APPLICATION(StudioApplication)
