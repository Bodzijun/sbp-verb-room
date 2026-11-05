// FFT, WAV I/O.
#pragma once
#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace verb {

using cd = std::complex<double>;

inline size_t nextPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// In-place radix-2 FFT. inverse=true scales by 1/N.
inline void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * 3.14159265358979323846 / (double)len * (inverse ? 1 : -1);
        cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= (double)n;
}

struct Wav {
    int sr = 48000;
    std::vector<std::vector<float>> ch;  // ch[channel][frame]
};

inline void writeWav(const std::string& path, const Wav& w) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    const uint16_t nch = (uint16_t)w.ch.size();
    const uint32_t frames = (uint32_t)w.ch[0].size();
    const uint32_t dataBytes = frames * nch * 4;
    auto w32 = [&](uint32_t v) { f.write((char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16(nch); w32((uint32_t)w.sr);
    w32((uint32_t)w.sr * nch * 4); w16(nch * 4); w16(32);
    f.write("data", 4); w32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < nch; ++c) f.write((char*)&w.ch[c][i], 4);
}

inline Wav readWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        throw std::runtime_error("not a WAV file: " + path);
    uint16_t fmt = 0, nch = 0, bits = 0;
    uint32_t sr = 0;
    size_t pos = 12;
    Wav w;
    while (pos + 8 <= d.size()) {
        uint32_t sz;
        std::memcpy(&sz, &d[pos + 4], 4);
        const char* id = &d[pos];
        const char* body = &d[pos + 8];
        if (!std::memcmp(id, "fmt ", 4)) {
            std::memcpy(&fmt, body, 2); std::memcpy(&nch, body + 2, 2);
            std::memcpy(&sr, body + 4, 4); std::memcpy(&bits, body + 14, 2);
            if (fmt == 0xFFFE) std::memcpy(&fmt, body + 24, 2);
        } else if (!std::memcmp(id, "data", 4)) {
            if (!nch) throw std::runtime_error("data before fmt");
            size_t avail = std::min<size_t>(sz, d.size() - pos - 8);
            size_t bps = bits / 8, frames = avail / (bps * nch);
            w.sr = (int)sr;
            w.ch.assign(nch, std::vector<float>(frames));
            for (size_t i = 0; i < frames; ++i)
                for (int c = 0; c < nch; ++c) {
                    const char* p = body + (i * nch + c) * bps;
                    float v;
                    if (fmt == 3 && bits == 32) std::memcpy(&v, p, 4);
                    else if (fmt == 1 && bits == 16) { int16_t x; std::memcpy(&x, p, 2); v = x / 32768.f; }
                    else if (fmt == 1 && bits == 24) { int32_t x = ((uint8_t)p[0] << 8) | ((uint8_t)p[1] << 16) | ((uint8_t)p[2] << 24); v = x / 2147483648.f; }
                    else if (fmt == 1 && bits == 32) { int32_t x; std::memcpy(&x, p, 4); v = x / 2147483648.f; }
                    else throw std::runtime_error("unsupported WAV format");
                    w.ch[c][i] = v;
                }
            return w;
        }
        pos += 8 + sz + (sz & 1);
    }
    throw std::runtime_error("no data chunk in " + path);
}

// Linear convolution via FFT (single block; fine for offline).
inline std::vector<float> convolve(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = a.size() + b.size() - 1, N = nextPow2(n);
    std::vector<cd> A(N), B(N);
    for (size_t i = 0; i < a.size(); ++i) A[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i) B[i] = b[i];
    fft(A, false); fft(B, false);
    for (size_t i = 0; i < N; ++i) A[i] *= B[i];
    fft(A, true);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = (float)A[i].real();
    return out;
}

}  // namespace verb
