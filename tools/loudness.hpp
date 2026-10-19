#pragma once
// ITU-R BS.1770 integrated loudness (K-weighting, 400 ms blocks, 75 % overlap, -70 LUFS absolute and -10 LU
// relative gates). Tools and tests only. The K filter uses the standard's analogue prototypes through the RBJ
// forms, which matches the 48 kHz table within ~0.05 dB.
#include <cmath>
#include <vector>

#include "fluent_audio/dsp.hpp"

namespace fa_tools {

inline double integratedLufs(const std::vector<std::vector<float>>& ch, int sr) {
    using namespace fluent_audio::dsp;
    const BiquadCoef shelf = rbj(BiquadType::kHighShelf, 1681.974450955533, 0.7071752369554196, 3.999843853973347, sr);
    const BiquadCoef hp = rbj(BiquadType::kHp, 38.13547087602444, 0.5003270373238773, 0, sr);
    const size_t n = ch.empty() ? 0 : ch[0].size();
    std::vector<double> power(n, 0.0);
    for (const auto& c : ch) {
        BiquadState a, b;
        for (size_t i = 0; i < n; ++i) {
            const double y = b.run(hp, a.run(shelf, c[i]));
            power[i] += y * y;
        }
    }
    const size_t block = static_cast<size_t>(0.4 * sr), hop = block / 4;
    std::vector<double> z;
    for (size_t s = 0; s + block <= n; s += hop) {
        double acc = 0;
        for (size_t i = s; i < s + block; ++i) acc += power[i];
        z.push_back(acc / block);
    }
    auto lufs = [](double p) { return -0.691 + 10.0 * std::log10(p + 1e-30); };
    double sum = 0;
    int cnt = 0;
    for (double p : z)
        if (lufs(p) > -70.0) { sum += p; ++cnt; }
    if (!cnt) return -120.0;
    const double rel = lufs(sum / cnt) - 10.0;
    sum = 0;
    cnt = 0;
    for (double p : z)
        if (lufs(p) > -70.0 && lufs(p) > rel) { sum += p; ++cnt; }
    return cnt ? lufs(sum / cnt) : -120.0;
}

}  // namespace fa_tools
