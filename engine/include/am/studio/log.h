// Auto Malign Studio: debug log. Thread-safe, keeps the last lines in memory for the UI and can mirror them to a file.
#pragma once
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace am::studio {

class Log {
public:
    enum Level { Debug, Info, Warn, Error };
    static Log& get() { static Log l; return l; }

    void setFile(const std::string& path) { std::lock_guard<std::mutex> g(m_); file_.close(); if (!path.empty()) file_.open(path, std::ios::app); }
    void setLevel(Level l) { std::lock_guard<std::mutex> g(m_); min_ = l; }
    void setEcho(bool on) { echo_ = on; }   // also print to stderr (command-line tool)

    void write(Level l, const std::string& msg) {
        if (l < min_) return;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        char head[48]; std::snprintf(head, sizeof head, "%9.3f %s ", t, l == Debug ? "DBG" : l == Info ? "INF" : l == Warn ? "WRN" : "ERR");
        const std::string line = head + msg;
        std::lock_guard<std::mutex> g(m_);
        lines_.push_back(line); ++serial_;
        while (lines_.size() > kKeep) lines_.pop_front();
        if (file_) { file_ << line << '\n'; file_.flush(); }
        if (echo_) std::fprintf(stderr, "%s\n", line.c_str());
    }
    // lines newer than `after` (a serial the caller got last time); returns the newest serial
    long since(long after, std::vector<std::string>& out) const {
        std::lock_guard<std::mutex> g(m_);
        const long first = serial_ - (long)lines_.size();
        for (long s = std::max(after, first); s < serial_; ++s) out.push_back(lines_[(size_t)(s - first)]);
        return serial_;
    }
    std::string all() const { std::lock_guard<std::mutex> g(m_); std::string s; for (auto& l : lines_) s += l + "\n"; return s; }
    void clear() { std::lock_guard<std::mutex> g(m_); lines_.clear(); }

private:
    static constexpr size_t kKeep = 4000;
    mutable std::mutex m_;
    std::deque<std::string> lines_;
    long serial_ = 0;
    std::ofstream file_;
    Level min_ = Debug;
    bool echo_ = false;
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

// AM_LOG(Info) << "loaded " << n << " stems";
struct LogLine {
    Log::Level l; std::ostringstream s;
    explicit LogLine(Log::Level lv) : l(lv) {}
    ~LogLine() { Log::get().write(l, s.str()); }
    template <class T> LogLine& operator<<(const T& v) { s << v; return *this; }
};
#define AM_LOG(level) ::am::studio::LogLine(::am::studio::Log::level)

} // namespace am::studio
