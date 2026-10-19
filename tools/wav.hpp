#pragma once
// Minimal WAV I/O for the tools and tests (PCM 16/24/32, float32; mono/stereo). Not part of the library.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fa_tools {

struct Wav {
    int sr = 48000;
    int channels = 1;
    std::vector<std::vector<float>> ch;   // non-interleaved
    size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

inline bool readWav(const std::string& path, Wav& w, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24)); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { err = path + ": not a WAV"; return false; }
    int fmt = 0, bits = 0;
    size_t pos = 12, data = 0, len = 0;
    while (pos + 8 <= d.size()) {
        const uint32_t sz = u32(pos + 4);
        if (!std::memcmp(d.data() + pos, "fmt ", 4)) {
            fmt = static_cast<int>(u16(pos + 8));
            w.channels = static_cast<int>(u16(pos + 10));
            w.sr = static_cast<int>(u32(pos + 12));
            bits = static_cast<int>(u16(pos + 22));
            if (fmt == 0xFFFE && sz >= 26) fmt = static_cast<int>(u16(pos + 32));
        } else if (!std::memcmp(d.data() + pos, "data", 4)) {
            data = pos + 8;
            len = std::min<size_t>(sz, d.size() - data);
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!data || w.channels < 1) { err = path + ": no data"; return false; }
    const int bps = bits / 8;
    const size_t frames = len / (bps * w.channels);
    w.ch.assign(w.channels, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < w.channels; ++c) {
            const size_t o = data + (i * w.channels + c) * bps;
            float v = 0;
            if (fmt == 3 && bits == 32) std::memcpy(&v, &d[o], 4);
            else if (bits == 16) v = static_cast<int16_t>(u16(o)) / 32768.0f;
            else if (bits == 24) v = static_cast<float>(static_cast<int32_t>((d[o] << 8) | (d[o + 1] << 16) | (static_cast<uint32_t>(d[o + 2]) << 24)) >> 8) / 8388608.0f;
            else if (bits == 32) v = static_cast<float>(static_cast<int32_t>(u32(o))) / 2147483648.0f;
            else { err = path + ": unsupported format"; return false; }
            w.ch[c][i] = v;
        }
    return true;
}

inline bool writeWav(const std::string& path, const Wav& w) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t frames = static_cast<uint32_t>(w.frames()), ch = static_cast<uint32_t>(w.channels);
    const uint32_t data = frames * ch * 4;
    auto put32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); put32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16); put16(3); put16(static_cast<uint16_t>(ch)); put32(static_cast<uint32_t>(w.sr));
    put32(static_cast<uint32_t>(w.sr) * ch * 4); put16(static_cast<uint16_t>(ch * 4)); put16(32);
    std::fwrite("data", 1, 4, f); put32(data);
    std::vector<float> inter(static_cast<size_t>(frames) * ch);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < ch; ++c) inter[i * ch + c] = w.ch[c][i];
    std::fwrite(inter.data(), 4, inter.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace fa_tools
