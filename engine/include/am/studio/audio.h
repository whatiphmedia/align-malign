// Auto Malign Studio (standalone session core): stereo buffers and WAV files. Framework free.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace am::studio {

// A stereo signal (mono files are loaded into both channels).
struct Stereo {
    std::vector<float> L, R;
    Stereo() = default;
    explicit Stereo(size_t n) : L(n, 0.f), R(n, 0.f) {}
    size_t size() const { return L.size(); }
    void resize(size_t n) { L.resize(n, 0.f); R.resize(n, 0.f); }
    float mid(size_t i) const { return 0.5f * (L[i] + R[i]); }
    std::vector<float> mono() const { std::vector<float> m(size()); for (size_t i = 0; i < size(); ++i) m[i] = mid(i); return m; }
    void add(const Stereo& o, float g = 1.f, size_t at = 0) {
        if (at + o.size() > size()) resize(at + o.size());
        for (size_t i = 0; i < o.size(); ++i) { L[at + i] += g * o.L[i]; R[at + i] += g * o.R[i]; }
    }
    void scale(float g) { for (auto& x : L) x *= g; for (auto& x : R) x *= g; }
    double energy() const { double e = 0; for (size_t i = 0; i < size(); ++i) e += (double)L[i] * L[i] + (double)R[i] * R[i]; return e; }
    float peak() const { float p = 0; for (size_t i = 0; i < size(); ++i) p = std::max({ p, std::abs(L[i]), std::abs(R[i]) }); return p; }
};

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }

struct WavInfo { int sampleRate = 0, channels = 0, bits = 0; bool isFloat = false; size_t frames = 0; std::string error; };

// Reads PCM 16/24/32-bit or 32/64-bit float WAV (incl. WAVE_FORMAT_EXTENSIBLE). Returns false with info.error set on failure.
inline bool readWav(const std::string& path, Stereo& out, WavInfo& info) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { info.error = "cannot open " + path; return false; }
    auto rd32 = [&](uint32_t& v) { return (bool)f.read((char*)&v, 4); };
    auto rd16 = [&](uint16_t& v) { return (bool)f.read((char*)&v, 2); };
    char id[4]; uint32_t sz;
    if (!f.read(id, 4) || std::memcmp(id, "RIFF", 4) || !rd32(sz) || !f.read(id, 4) || std::memcmp(id, "WAVE", 4)) { info.error = "not a WAV file"; return false; }
    uint16_t fmt = 0, ch = 0, bits = 0; uint32_t sr = 0; bool haveFmt = false;
    while (f.read(id, 4) && rd32(sz)) {
        if (!std::memcmp(id, "fmt ", 4)) {
            uint32_t byteRate; uint16_t align; rd16(fmt); rd16(ch); rd32(sr); rd32(byteRate); rd16(align); rd16(bits);
            uint32_t rest = sz - 16;
            if (fmt == 0xFFFE && rest >= 24) {   // extensible: the sub-format GUID starts with the real format code
                uint16_t cb, valid; uint32_t mask; rd16(cb); rd16(valid); rd32(mask); uint16_t sub; rd16(sub); fmt = sub; rest -= 10;
            }
            f.seekg(rest + (sz & 1), std::ios::cur); haveFmt = true;
        } else if (!std::memcmp(id, "data", 4)) {
            if (!haveFmt) { info.error = "data before fmt"; return false; }
            if (ch < 1 || (fmt != 1 && fmt != 3) || (fmt == 1 && bits != 16 && bits != 24 && bits != 32) || (fmt == 3 && bits != 32 && bits != 64)) {
                info.error = "unsupported WAV format (code " + std::to_string(fmt) + ", " + std::to_string(bits) + " bit)"; return false;
            }
            const int bps = bits / 8; const size_t frames = sz / (bps * ch);
            std::vector<char> raw((size_t)frames * bps * ch);
            f.read(raw.data(), (std::streamsize)raw.size());
            const size_t got = (size_t)f.gcount() / (bps * ch);
            out = Stereo(got);
            auto sample = [&](const char* p) -> float {
                if (fmt == 3) { if (bits == 32) { float v; std::memcpy(&v, p, 4); return v; } double v; std::memcpy(&v, p, 8); return (float)v; }
                if (bits == 16) { int16_t v; std::memcpy(&v, p, 2); return v / 32768.f; }
                if (bits == 24) { int32_t v = (int32_t)((uint32_t)(uint8_t)p[0] << 8 | (uint32_t)(uint8_t)p[1] << 16 | (uint32_t)(uint8_t)p[2] << 24); return (float)(v / 2147483648.0); }
                int32_t v; std::memcpy(&v, p, 4); return (float)(v / 2147483648.0);
            };
            for (size_t i = 0; i < got; ++i) {
                const char* p = raw.data() + i * bps * ch;
                out.L[i] = sample(p); out.R[i] = ch > 1 ? sample(p + bps) : out.L[i];
            }
            info.sampleRate = (int)sr; info.channels = ch; info.bits = bits; info.isFloat = fmt == 3; info.frames = got;
            return true;
        } else f.seekg(sz + (sz & 1), std::ios::cur);
    }
    info.error = "no audio data"; return false;
}

// Writes a stereo WAV: 24-bit PCM (default) or 32-bit float.
inline bool writeWav(const std::string& path, const Stereo& s, int sampleRate, bool asFloat = false) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint16_t ch = 2, bits = asFloat ? 32 : 24, fmt = asFloat ? 3 : 1; const uint32_t bps = bits / 8;
    const uint32_t data = (uint32_t)(s.size() * ch * bps);
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); }; auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + data); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(fmt); w16(ch); w32((uint32_t)sampleRate); w32((uint32_t)sampleRate * ch * bps); w16((uint16_t)(ch * bps)); w16(bits);
    f.write("data", 4); w32(data);
    std::vector<char> buf(s.size() * ch * bps);
    char* p = buf.data();
    for (size_t i = 0; i < s.size(); ++i)
        for (int c = 0; c < 2; ++c) {
            const float v = c ? s.R[i] : s.L[i];
            if (asFloat) { std::memcpy(p, &v, 4); p += 4; }
            else {
                const double d = std::clamp((double)v, -1.0, 1.0 - 1.0 / 8388608.0);
                const int32_t q = (int32_t)std::lround(d * 8388608.0);
                p[0] = (char)(q & 0xff); p[1] = (char)((q >> 8) & 0xff); p[2] = (char)((q >> 16) & 0xff); p += 3;
            }
        }
    f.write(buf.data(), (std::streamsize)buf.size());
    return (bool)f;
}

} // namespace am::studio
