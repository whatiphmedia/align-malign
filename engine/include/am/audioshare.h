// Shares each Auto Malign instance's input with the others, indexed by timeline sample, so one track can use
// another track's sound (Phantom notes > Source) without any host routing. A reader asks for samples at least the
// plugin latency in the past, which every instance in the cycle has written by then.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace am {

class SharedRing {
public:
    static constexpr int kSize = 1 << 17;                       // ~2.7 s at 48 kHz
    SharedRing() : L_(kSize, 0.f), R_(kSize, 0.f) {}

    // audio thread of the owner: input at timeline samples [pos, pos+n)
    void write(const float* l, const float* r, int n, int64_t pos) {
        if (pos < 0 || n <= 0) return;
        if (pos != end_.load(std::memory_order_relaxed)) start_.store(pos, std::memory_order_relaxed);  // a jump: only what follows is valid
        for (int i = 0; i < n; ++i) { const size_t k = (size_t)((pos + i) & (kSize - 1)); L_[k] = l[i]; R_[k] = r ? r[i] : l[i]; }
        end_.store(pos + n, std::memory_order_release);
    }
    // any audio thread: samples [pos, pos+n); false (and silence) if they are not there (yet, or any more)
    bool read(int64_t pos, int n, float* l, float* r) const {
        const int64_t e = end_.load(std::memory_order_acquire), s = std::max(start_.load(std::memory_order_relaxed), e - (int64_t)kSize + 4096);
        if (pos < s || pos + n > e) { std::fill(l, l + n, 0.f); std::fill(r, r + n, 0.f); return false; }
        for (int i = 0; i < n; ++i) { const size_t k = (size_t)((pos + i) & (kSize - 1)); l[i] = L_[k]; r[i] = R_[k]; }
        return true;
    }

private:
    std::vector<float> L_, R_;
    std::atomic<int64_t> start_{ INT64_MAX }, end_{ INT64_MIN / 2 };
};

class AudioShare {
public:
    static AudioShare& get() { static AudioShare a; return a; }
    std::shared_ptr<SharedRing> publish(int id) {
        std::lock_guard<std::mutex> g(m_);
        auto& r = rings_[id];
        if (!r) r = std::make_shared<SharedRing>();
        return r;
    }
    void withdraw(int id) { std::lock_guard<std::mutex> g(m_); rings_.erase(id); }
    std::shared_ptr<SharedRing> find(int id) const {
        std::lock_guard<std::mutex> g(m_);
        auto it = rings_.find(id);
        return it == rings_.end() ? nullptr : it->second;
    }

private:
    mutable std::mutex m_;
    std::map<int, std::shared_ptr<SharedRing>> rings_;
};

} // namespace am
