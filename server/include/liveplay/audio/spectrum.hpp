// ============================================================================
// liveplay/audio/spectrum.hpp
// ----------------------------------------------------------------------------
// The analyser's maths: a window of samples in, a log-frequency magnitude
// spectrum out. Runs on the broadcast thread, never the audio thread.
//
// A plain iterative radix-2 FFT. Nothing is vendored for this, and at one
// 4096-point transform per armed tap per meter tick (~30 Hz) a textbook FFT
// costs a fraction of a millisecond; pulling in a library for it would be
// weight without benefit.
//
// Output is `bins` bands spaced logarithmically from f_lo to f_hi, each the
// PEAK magnitude of the FFT lines inside it (in dBFS, a full-scale sine reads
// 0 dB). Low bands narrower than one FFT line take the line they fall on, so
// the bottom octave is coarse — the price of a 4096 window at 48 kHz
// (11.7 Hz lines), and the same trade every analyser of this size makes.
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace liveplay::audio {

class SpectrumAnalyser {
public:
    // n must be a power of two.
    explicit SpectrumAnalyser(std::size_t n = 4096) : n_(n), window_(n), buf_(n) {
        double sum = 0.0;
        for (std::size_t i = 0; i < n_; ++i) {
            // Hann: good leakage for music, and the usual analyser choice.
            window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                              static_cast<double>(n_ - 1));
            sum += window_[i];
        }
        // A sine of amplitude 1 puts sum/2 into its line; scale that to 1.
        scale_ = 2.0 / sum;
    }

    std::size_t size() const noexcept { return n_; }

    // Log-spaced band centres, for a client that wants to label the axis.
    static std::vector<float> band_edges(std::size_t bins, double f_lo, double f_hi) {
        std::vector<float> edges(bins + 1);
        const double ratio = std::log(f_hi / f_lo);
        for (std::size_t b = 0; b <= bins; ++b)
            edges[b] = static_cast<float>(f_lo * std::exp(ratio * static_cast<double>(b) /
                                                          static_cast<double>(bins)));
        return edges;
    }

    // samples: n values. out: `bins` values in dBFS (floored at -120).
    void analyse(const float* samples, double sample_rate, std::size_t bins,
                 double f_lo, double f_hi, float* out) {
        for (std::size_t i = 0; i < n_; ++i) buf_[i] = {samples[i] * window_[i], 0.0};
        fft(buf_);
        const std::size_t half = n_ / 2;
        const double hz_per_line = sample_rate / static_cast<double>(n_);
        const double ratio = std::log(f_hi / f_lo);
        for (std::size_t b = 0; b < bins; ++b) {
            const double lo = f_lo * std::exp(ratio * static_cast<double>(b) / static_cast<double>(bins));
            const double hi = f_lo * std::exp(ratio * static_cast<double>(b + 1) / static_cast<double>(bins));
            std::size_t k0 = static_cast<std::size_t>(std::floor(lo / hz_per_line + 0.5));
            std::size_t k1 = static_cast<std::size_t>(std::floor(hi / hz_per_line + 0.5));
            k0 = std::clamp<std::size_t>(k0, 1, half - 1);
            k1 = std::clamp<std::size_t>(k1, k0, half - 1);
            double peak = 0.0;
            for (std::size_t k = k0; k <= k1; ++k) peak = std::max(peak, std::abs(buf_[k]));
            const double mag = peak * scale_;
            out[b] = mag > 1e-6 ? static_cast<float>(20.0 * std::log10(mag)) : -120.0f;
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    static void fft(std::vector<std::complex<double>>& a) {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const double ang = -2.0 * kPi / static_cast<double>(len);
            const std::complex<double> wlen(std::cos(ang), std::sin(ang));
            for (std::size_t i = 0; i < n; i += len) {
                std::complex<double> w(1.0, 0.0);
                for (std::size_t j = 0; j < len / 2; ++j) {
                    const auto u = a[i + j];
                    const auto v = a[i + j + len / 2] * w;
                    a[i + j]           = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
    }

    std::size_t n_;
    std::vector<double> window_;
    std::vector<std::complex<double>> buf_;
    double scale_ = 1.0;
};

} // namespace liveplay::audio
