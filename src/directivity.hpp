#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>

namespace verb {

constexpr int kDirectivityBands = 7;
constexpr int kDefaultDirectivityOrder = 4;
constexpr int kMaxDirectivityOrder = 8;
constexpr int kMaxDirectivityCoeffs = (kMaxDirectivityOrder + 1) * (kMaxDirectivityOrder + 1);
constexpr size_t kMaxVerbdirBytes = 65536;
constexpr double kDirectivityPi = 3.14159265358979323846;

inline int directivityNumCoeffs(int order) {
    return (order + 1) * (order + 1);
}

namespace detail {

struct ShNormTable {
    double norm[kMaxDirectivityOrder + 1][kMaxDirectivityOrder + 1]{};
    double dblFact[kMaxDirectivityOrder + 1]{};

    ShNormTable() {
        dblFact[0] = 1.0;
        for (int m = 1; m <= kMaxDirectivityOrder; ++m)
            dblFact[m] = dblFact[m - 1] * (double)(2 * m - 1);

        for (int l = 0; l <= kMaxDirectivityOrder; ++l) {
            norm[l][0] = std::sqrt((double)(2 * l + 1));
            double ratio = 1.0;
            for (int m = 1; m <= l; ++m) {
                ratio /= (double)(l - m + 1) * (double)(l + m);
                norm[l][m] = std::sqrt(2.0 * (double)(2 * l + 1) * ratio);
            }
        }
    }
};

inline const ShNormTable& shNorms() {
    static const ShNormTable t;
    return t;
}

}  // namespace detail

// Evaluate 4*pi-orthonormal Real Spherical Harmonics Y_{l,m}(ux, uy, uz) for l = 0..order, m = -l..l
// in ACN order k = l*l + l + m. Input (ux, uy, uz) must be a unit vector in the source's local frame
// (+x forward, +y left, +z up). Uses pure polynomial recurrence with zero trig/sqrt calls.
inline void evalRealSH(int order, double ux, double uy, double uz, double* out) {
    order = std::clamp(order, 0, kMaxDirectivityOrder);
    const auto& tbl = detail::shNorms();

    double C[kMaxDirectivityOrder + 1];
    double S[kMaxDirectivityOrder + 1];
    C[0] = 1.0;
    S[0] = 0.0;
    for (int m = 1; m <= order; ++m) {
        C[m] = ux * C[m - 1] - uy * S[m - 1];
        S[m] = uy * C[m - 1] + ux * S[m - 1];
    }

    for (int m = 0; m <= order; ++m) {
        double q_mm = tbl.dblFact[m];
        if (m == 0) {
            out[0] = tbl.norm[0][0] * q_mm;
        } else {
            const int base = m * m + m;
            const double nm = tbl.norm[m][m] * q_mm;
            out[base + m] = nm * C[m];
            out[base - m] = nm * S[m];
        }
        if (m + 1 <= order) {
            double q_mp1 = (double)(2 * m + 1) * uz * q_mm;
            const int l = m + 1;
            const int base = l * l + l;
            if (m == 0) {
                out[base] = tbl.norm[l][0] * q_mp1;
            } else {
                const double nm = tbl.norm[l][m] * q_mp1;
                out[base + m] = nm * C[m];
                out[base - m] = nm * S[m];
            }
            double q_prev2 = q_mm;
            double q_prev1 = q_mp1;
            for (int ll = m + 2; ll <= order; ++ll) {
                const double q_cur = ((double)(2 * ll - 1) * uz * q_prev1 - (double)(ll + m - 1) * q_prev2) / (double)(ll - m);
                const int b = ll * ll + ll;
                if (m == 0) {
                    out[b] = tbl.norm[ll][0] * q_cur;
                } else {
                    const double nm = tbl.norm[ll][m] * q_cur;
                    out[b + m] = nm * C[m];
                    out[b - m] = nm * S[m];
                }
                q_prev2 = q_prev1;
                q_prev1 = q_cur;
            }
        }
    }
}

struct QuadratureNode {
    double ux = 1.0, uy = 0.0, uz = 0.0, weight = 0.0;
};

// 24-point Gauss-Legendre in z = sin(el) in [-1, 1] x 48-point uniform azimuth in [0, 2*pi).
// Weights sum to 1.0 (representing (1 / 4*pi) dOmega). Exact for spherical polynomials up to degree 47.
inline const std::vector<QuadratureNode>& sphericalQuadratureNodes() {
    static const std::vector<QuadratureNode> nodes = []() {
        constexpr int nZ = 24;
        constexpr int nPhi = 48;
        double zNodes[nZ]{};
        double zWeights[nZ]{};
        for (int i = 0; i < nZ / 2; ++i) {
            double x = std::cos(kDirectivityPi * ((double)i + 0.75) / ((double)nZ + 0.5));
            for (int it = 0; it < 30; ++it) {
                double p0 = 1.0, p1 = x;
                for (int k = 2; k <= nZ; ++k) {
                    double pk = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / (double)k;
                    p0 = p1;
                    p1 = pk;
                }
                double dp = (double)nZ * (x * p1 - p0) / (x * x - 1.0);
                double dx = p1 / dp;
                x -= dx;
                if (std::fabs(dx) < 1e-15) break;
            }
            double p0 = 1.0, p1 = x;
            for (int k = 2; k <= nZ; ++k) {
                double pk = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / (double)k;
                p0 = p1;
                p1 = pk;
            }
            double dp = (double)nZ * (x * p1 - p0) / (x * x - 1.0);
            double w = 2.0 / ((1.0 - x * x) * dp * dp);
            zNodes[i] = -x;
            zWeights[i] = w;
            zNodes[nZ - 1 - i] = x;
            zWeights[nZ - 1 - i] = w;
        }
        std::vector<QuadratureNode> out;
        out.reserve(nZ * nPhi);
        for (int iz = 0; iz < nZ; ++iz) {
            const double uz = zNodes[iz];
            const double rxy = std::sqrt(std::max(0.0, 1.0 - uz * uz));
            const double wz = 0.5 * zWeights[iz] / (double)nPhi;
            for (int ip = 0; ip < nPhi; ++ip) {
                const double phi = 2.0 * kDirectivityPi * (double)ip / (double)nPhi;
                out.push_back({rxy * std::cos(phi), rxy * std::sin(phi), uz, wz});
            }
        }
        return out;
    }();
    return nodes;
}

// Transform a unit direction vector from the source's local frame (+x forward, +y left, +z up)
// into the profile's unrotated frame by applying the inverse of the active profile-aim correction
// R_z(aimAzDeg) * R_el(aimElDeg) * R_x(aimRollDeg).
inline void applyProfileAim(double aimAzDeg, double aimElDeg, double aimRollDeg,
                            double& ux, double& uy, double& uz) noexcept {
    if (aimAzDeg == 0.0 && aimElDeg == 0.0 && aimRollDeg == 0.0) return;
    constexpr double kDegToRad = kDirectivityPi / 180.0;
    if (aimAzDeg != 0.0) {
        const double a = aimAzDeg * kDegToRad;
        const double ca = std::cos(a), sa = std::sin(a);
        const double nx = ux * ca + uy * sa;
        const double ny = -ux * sa + uy * ca;
        ux = nx;
        uy = ny;
    }
    if (aimElDeg != 0.0) {
        const double e = aimElDeg * kDegToRad;
        const double ce = std::cos(e), se = std::sin(e);
        const double nx = ux * ce + uz * se;
        const double nz = -ux * se + uz * ce;
        ux = nx;
        uz = nz;
    }
    if (aimRollDeg != 0.0) {
        const double r = aimRollDeg * kDegToRad;
        const double cr = std::cos(r), sr = std::sin(r);
        const double ny = uy * cr + uz * sr;
        const double nz = -uy * sr + uz * cr;
        uy = ny;
        uz = nz;
    }
    const double r2 = ux * ux + uy * uy + uz * uz;
    if (r2 > 1e-24 && std::fabs(r2 - 1.0) > 1e-6) {
        const double invR = 1.0 / std::sqrt(r2);
        ux *= invR;
        uy *= invR;
        uz *= invR;
    }
}

struct DirectivityProfile {
    std::string id;
    std::string name;
    std::string category = "Custom";
    std::string license = "Custom";
    std::string attribution;
    bool isModel = false;
    int order = kDefaultDirectivityOrder;
    std::array<std::vector<float>, kDirectivityBands> coeffs{};
    double minGain = 1e-3;  // -60 dB floor

    bool valid() const {
        if (id.empty() || name.empty() || order < 0 || order > kMaxDirectivityOrder) return false;
        const size_t n = (size_t)directivityNumCoeffs(order);
        for (int b = 0; b < kDirectivityBands; ++b)
            if (coeffs[(size_t)b].size() != n) return false;
        return true;
    }

    double evalBandWithBasis(int band, const double* y, int nCoeffs) const {
        const auto& c = coeffs[(size_t)band];
        double sum = 0.0;
        for (int k = 0; k < nCoeffs; ++k) sum += (double)c[(size_t)k] * y[k];
        return std::max(minGain, sum);
    }

    double evalBandLocal(int band, double ux, double uy, double uz) const {
        double y[kMaxDirectivityCoeffs];
        evalRealSH(order, ux, uy, uz, y);
        return evalBandWithBasis(band, y, directivityNumCoeffs(order));
    }

    std::array<double, kDirectivityBands> evalLocal(double ux, double uy, double uz) const {
        double y[kMaxDirectivityCoeffs];
        const int n = directivityNumCoeffs(order);
        evalRealSH(order, ux, uy, uz, y);
        std::array<double, kDirectivityBands> out{};
        for (int b = 0; b < kDirectivityBands; ++b)
            out[(size_t)b] = evalBandWithBasis(b, y, n);
        return out;
    }

    std::vector<float> horizontalSlice(int band, int numAngles = 72, double amount = 1.0,
                                       double aimAzDeg = 0.0, double aimElDeg = 0.0, double aimRollDeg = 0.0) const {
        band = std::clamp(band, 0, kDirectivityBands - 1);
        numAngles = std::clamp(numAngles, 8, 360);
        const double w = std::clamp(amount, 0.0, 1.0);
        std::vector<float> slice((size_t)numAngles, 1.0f);
        if (!valid()) return slice;
        for (int i = 0; i < numAngles; ++i) {
            const double az = 2.0 * kDirectivityPi * (double)i / (double)numAngles;
            double ux = std::cos(az), uy = std::sin(az), uz = 0.0;
            applyProfileAim(aimAzDeg, aimElDeg, aimRollDeg, ux, uy, uz);
            const double g = evalBandLocal(band, ux, uy, uz);
            slice[(size_t)i] = (float)std::sqrt((1.0 - w) + w * g * g);
        }
        return slice;
    }
};


// Single audio consumer, serialized non-audio producers. Slots are owned by exactly
// one of front (audio), middle (atomic exchange), back (producer). Only the
// producer assigns shared_ptrs; audio reads references and never releases owners.
struct DirectivityState {
    std::shared_ptr<const DirectivityProfile> profile;
    double amount = 1.0, pattern = 0.0;
    bool fixedPattern = false;
    double aimAzDeg = 0.0, aimElDeg = 0.0, aimRollDeg = 0.0;
};
class DirectivityMailbox {
    mutable std::mutex mutex_;
    std::array<DirectivityState, 3> slots_{};
    DirectivityState control_;
    std::atomic<unsigned> middle_{1};
    unsigned back_ = 2, front_ = 0;
public:
    bool publish(DirectivityState state) { // non-audio only; also retires old owners here
        std::lock_guard<std::mutex> lock(mutex_);
        if (state.profile == control_.profile && state.amount == control_.amount
            && state.pattern == control_.pattern && state.fixedPattern == control_.fixedPattern
            && state.aimAzDeg == control_.aimAzDeg && state.aimElDeg == control_.aimElDeg
            && state.aimRollDeg == control_.aimRollDeg) return false;
        control_ = std::move(state);
        slots_[back_] = control_;
        back_ = middle_.exchange(back_ | 4u, std::memory_order_acq_rel) & 3u;
        return true;
    }
    const DirectivityState& audioRead() noexcept {
        if (middle_.load(std::memory_order_acquire) & 4u)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3u;
        return slots_[front_];
    }
    DirectivityState controlRead() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return control_;
    }
};

// Mean spherical power (1 / 4*pi) * integral_{S^2} g_b(u)^2 dOmega
inline double bandMeanPower(const DirectivityProfile& p, int band) {
    if (!p.valid() || band < 0 || band >= kDirectivityBands) return 0.0;
    const auto& nodes = sphericalQuadratureNodes();
    const int n = directivityNumCoeffs(p.order);
    double y[kMaxDirectivityCoeffs];
    double power = 0.0;
    for (const auto& q : nodes) {
        evalRealSH(p.order, q.ux, q.uy, q.uz, y);
        const double g = p.evalBandWithBasis(band, y, n);
        power += q.weight * (g * g);
    }
    return power;
}

// Normalize every octave band of `p` to unit spherical power: (1 / 4*pi) * integral g_b(u)^2 dOmega = 1.
inline bool normalizeProfilePower(DirectivityProfile& p, std::string* err = nullptr) {
    if (!p.valid()) {
        if (err) *err = "Directivity profile structure is invalid.";
        return false;
    }
    for (int b = 0; b < kDirectivityBands; ++b) {
        auto& c = p.coeffs[(size_t)b];
        double l2 = 0.0;
        for (float v : c) {
            if (!std::isfinite(v)) {
                if (err) *err = "Band " + std::to_string(b) + " contains non-finite SH coefficients.";
                return false;
            }
            l2 += (double)v * (double)v;
        }
        if (l2 < 1e-12) {
            if (err) *err = "Band " + std::to_string(b) + " has zero energy.";
            return false;
        }
        const double invL2 = 1.0 / std::sqrt(l2);
        for (float& v : c) v = (float)((double)v * invL2);

        // The floor must not masquerade as real emitted energy.
        double positivePower = 0.;
        for (const auto& q : sphericalQuadratureNodes()) {
            double y[kMaxDirectivityCoeffs]; evalRealSH(p.order,q.ux,q.uy,q.uz,y);
            double raw=0.; for(size_t k=0;k<c.size();++k) raw+=(double)c[k]*y[k];
            positivePower += q.weight*std::max(0.,raw)*std::max(0.,raw);
        }
        if (!(positivePower > 1e-10)) {
            if (err) *err = "Band " + std::to_string(b) + " has no usable positive radiation.";
            return false;
        }
        for (int iter = 0; iter < 4; ++iter) {
            const double pwr = bandMeanPower(p, b);
            if (!(pwr > 1e-12) || !std::isfinite(pwr)) {
                if (err) *err = "Band " + std::to_string(b) + " failed unit-power normalization.";
                return false;
            }
            if (std::fabs(pwr - 1.0) < 1e-7) break;
            const double scale = 1.0 / std::sqrt(pwr);
            for (float& v : c) v = (float)((double)v * scale);
        }
    }
    for (int b=0;b<kDirectivityBands;++b) {
        const double power=bandMeanPower(p,b);
        if (!std::isfinite(power) || std::abs(power-1.)>1e-4
            || std::any_of(p.coeffs[b].begin(),p.coeffs[b].end(),[](float v){return !std::isfinite(v);})) {
            if(err)*err="Post-clipping unit-power normalization did not converge.";
            return false;
        }
    }
    return true;
}

// Closest first-order analytic pattern parameter p in [0, 1] for backward compatibility.
inline double fitAnalyticPattern(const DirectivityProfile& p, double amount = 1.0,
                                 double aimAzDeg = 0.0, double aimElDeg = 0.0, double aimRollDeg = 0.0) {
    if (!p.valid()) return 0.0;
    const double w = std::clamp(amount, 0.0, 1.0);
    if (w <= 1e-6) return 0.0;
    double fx = 1.0, fy = 0.0, fz = 0.0;
    double bx = -1.0, by = 0.0, bz = 0.0;
    applyProfileAim(aimAzDeg, aimElDeg, aimRollDeg, fx, fy, fz);
    applyProfileAim(aimAzDeg, aimElDeg, aimRollDeg, bx, by, bz);
    // Compare front (+x) vs rear (-x) gain in mid bands (500 Hz, 1 kHz, 2 kHz)
    double ratioSum = 0.0;
    for (int b : {2, 3, 4}) {
        const double gf0 = p.evalBandLocal(b, fx, fy, fz);
        const double gb0 = p.evalBandLocal(b, bx, by, bz);
        const double gf = std::sqrt((1.0 - w) + w * gf0 * gf0);
        const double gb = std::sqrt((1.0 - w) + w * gb0 * gb0);
        const double r = (gf - gb) / std::max(1e-6, gf + gb);
        ratioSum += std::clamp(0.5 * r, 0.0, 1.0);
    }
    return std::clamp(ratioSum / 3.0, 0.0, 1.0);
}

namespace detail {

inline std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline bool validProfileId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// Compute derivative of spherical Hankel function of first kind h_l^(1)'(x) = j_l'(x) + i * y_l'(x)
inline std::complex<double> sphericalHankel1Prime(int l, double x) {
    x = std::max(1e-4, x);
    double j_prev = std::sin(x) / x;
    double y_prev = -std::cos(x) / x;
    if (l == 0) {
        const double jp = std::cos(x) / x - std::sin(x) / (x * x);
        const double yp = std::sin(x) / x + std::cos(x) / (x * x);
        return {jp, yp};
    }
    double j_cur = std::sin(x) / (x * x) - std::cos(x) / x;
    double y_cur = -std::cos(x) / (x * x) - std::sin(x) / x;
    for (int n = 1; n < l; ++n) {
        const double j_next = ((double)(2 * n + 1) / x) * j_cur - j_prev;
        const double y_next = ((double)(2 * n + 1) / x) * y_cur - y_prev;
        j_prev = j_cur;
        j_cur = j_next;
        y_prev = y_cur;
        y_cur = y_next;
    }
    const double jp = j_prev - ((double)(l + 1) / x) * j_cur;
    const double yp = y_prev - ((double)(l + 1) / x) * y_cur;
    return {jp, yp};
}

inline double legendreP(int l, double x) {
    if (l < 0) return 1.0;
    if (l == 0) return 1.0;
    if (l == 1) return x;
    double p0 = 1.0, p1 = x;
    for (int n = 2; n <= l; ++n) {
        const double pn = ((double)(2 * n - 1) * x * p1 - (double)(n - 1) * p0) / (double)n;
        p0 = p1;
        p1 = pn;
    }
    return p1;
}

// Far-field pressure magnitude for a vibrating spherical cap of half-angle alpha0 on a rigid sphere of radius sphereR
// (Morse & Ingard, Theoretical Acoustics, Section 7.2: Radiation from a Piston in a Sphere).
inline double sphereCapFarFieldMag(double cosGamma, double kR, double cosAlpha0, int maxSeriesL = 18) {
    std::complex<double> sum(0.0, 0.0);
    std::complex<double> negI_pow(1.0, 0.0);
    for (int l = 0; l <= maxSeriesL; ++l) {
        const double w_l = (l == 0) ? (1.0 - cosAlpha0) * 0.5
                                    : 0.5 * (legendreP(l - 1, cosAlpha0) - legendreP(l + 1, cosAlpha0));
        const auto hp = sphericalHankel1Prime(l, kR);
        if (std::abs(hp) < 1e-30) break;
        const double pl = legendreP(l, cosGamma);
        const std::complex<double> term = (w_l * pl) * negI_pow / hp;
        sum += term;
        if (l > (int)(2.0 * kR + 4.0) && std::abs(term) < 1e-6 * std::abs(sum)) break;
        negI_pow *= std::complex<double>(0.0, -1.0);
    }
    return std::max(1e-3, std::abs(sum));
}

}  // namespace detail

// Build an analytic directivity profile from the exact spherical-cap-in-rigid-sphere model (Morse & Ingard).
// Used for:
// - Mouth on a rigid sphere (voice): sphereRadiusM = 0.0875 m (human head), apertureRadiusM = 0.015 m (mouth).
// - Piston in a sphere (brass bell): apertureRadiusM = bell radius a, sphereRadiusM = a / sin(alpha0).
inline DirectivityProfile buildSphereCapDirectivity(
    const std::string& id,
    const std::string& name,
    const std::string& category,
    double sphereRadiusM,
    double apertureRadiusM,
    const std::string& attribution,
    int order = kDefaultDirectivityOrder)
{
    DirectivityProfile p;
    p.id = id;
    p.name = name;
    p.category = category;
    p.license = "First-principles acoustic model (CC0 / Public Domain)";
    p.attribution = attribution;
    p.isModel = true;
    p.order = std::clamp(order, 1, kMaxDirectivityOrder);

    const double sinAlpha = std::clamp(apertureRadiusM / std::max(1e-3, sphereRadiusM), 0.05, 0.95);
    const double cosAlpha0 = std::sqrt(1.0 - sinAlpha * sinAlpha);
    constexpr double kSoundSpeed = 343.0;
    constexpr double kBandFc[kDirectivityBands] = {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};
    constexpr double kSubFactors[5] = {0.75785828, 0.87055056, 1.0, 1.14869835, 1.31950791};

    const auto& nodes = sphericalQuadratureNodes();
    const int nCoeffs = directivityNumCoeffs(p.order);

    for (int b = 0; b < kDirectivityBands; ++b) {
        p.coeffs[(size_t)b].assign((size_t)nCoeffs, 0.0f);
        std::vector<double> cAccum((size_t)nCoeffs, 0.0);
        double y[kMaxDirectivityCoeffs];

        for (const auto& q : nodes) {
            // Axisymmetric around +x (ux = cos(gamma)), octave-band RMS averaged over 5 log-spaced sub-frequencies
            double pwrSum = 0.0;
            for (double sf : kSubFactors) {
                const double freq = kBandFc[b] * sf;
                const double kR = (2.0 * kDirectivityPi * freq / kSoundSpeed) * sphereRadiusM;
                const double mag = detail::sphereCapFarFieldMag(q.ux, kR, cosAlpha0);
                pwrSum += mag * mag;
            }
            const double g = std::sqrt(pwrSum / 5.0);
            evalRealSH(p.order, q.ux, q.uy, q.uz, y);
            for (int k = 0; k < nCoeffs; ++k)
                cAccum[(size_t)k] += q.weight * g * y[k];
        }
        for (int k = 0; k < nCoeffs; ++k)
            p.coeffs[(size_t)b][(size_t)k] = (float)cAccum[(size_t)k];
    }
    normalizeProfilePower(p);
    return p;
}

// Parse a `.verbdir` v1 text representation with strict validation and clear line-numbered error messages.
inline bool parseVerbdir(const std::string& text, DirectivityProfile& out, std::string& err) {
    err.clear();
    if (text.empty()) {
        err = "Empty .verbdir file.";
        return false;
    }
    if (text.size() > kMaxVerbdirBytes) {
        err = ".verbdir file exceeds maximum size of 65536 bytes.";
        return false;
    }

    DirectivityProfile p;
    p.id.clear();
    p.name.clear();
    p.order = -1;

    std::istringstream in(text);
    std::string rawLine;
    int lineNo = 0;
    bool seenHeader = false;
    bool seenBands = false;
    std::array<bool, kDirectivityBands> bandSeen{};

    while (std::getline(in, rawLine)) {
        ++lineNo;
        const std::string line = detail::trimStr(rawLine);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        std::string key;
        ls >> key;

        if (!seenHeader) {
            int ver = 0;
            std::string extra;
            if (key != "VERBDIR" || !(ls >> ver) || (ls >> extra) || ver != 1) {
                err = "Line " + std::to_string(lineNo) + ": expected 'VERBDIR 1' header.";
                return false;
            }
            seenHeader = true;
            continue;
        }

        if (key == "id") {
            std::string val, extra;
            if (!(ls >> val) || (ls >> extra)) {
                err = "Line " + std::to_string(lineNo) + ": 'id' requires a single token.";
                return false;
            }
            if (!detail::validProfileId(val) || val == "none") {
                err = "Line " + std::to_string(lineNo) + ": invalid profile id '" + val + "'.";
                return false;
            }
            p.id = val;
        } else if (key == "name") {
            std::string rest;
            std::getline(ls, rest);
            rest = detail::trimStr(rest);
            if (rest.empty() || rest.size() > 128) {
                err = "Line " + std::to_string(lineNo) + ": 'name' must be 1..128 characters.";
                return false;
            }
            p.name = rest;
        } else if (key == "category") {
            std::string rest;
            std::getline(ls, rest);
            rest = detail::trimStr(rest);
            if (rest.empty() || rest.size() > 64) {
                err = "Line " + std::to_string(lineNo) + ": 'category' must be 1..64 characters.";
                return false;
            }
            p.category = rest;
        } else if (key == "license") {
            std::string rest;
            std::getline(ls, rest);
            rest = detail::trimStr(rest);
            if (rest.empty() || rest.size() > 128) {
                err = "Line " + std::to_string(lineNo) + ": 'license' must be 1..128 characters.";
                return false;
            }
            p.license = rest;
        } else if (key == "attribution") {
            std::string rest;
            std::getline(ls, rest);
            rest = detail::trimStr(rest);
            if (rest.size() > 512) {
                err = "Line " + std::to_string(lineNo) + ": 'attribution' exceeds 512 characters.";
                return false;
            }
            p.attribution = rest;
        } else if (key == "model") {
            int flag = 0;
            std::string extra;
            if (!(ls >> flag) || (ls >> extra) || (flag != 0 && flag != 1)) {
                err = "Line " + std::to_string(lineNo) + ": 'model' must be 0 or 1.";
                return false;
            }
            p.isModel = (flag == 1);
        } else if (key == "order") {
            int ord = -1;
            std::string extra;
            if (!(ls >> ord) || (ls >> extra) || ord < 0 || ord > kMaxDirectivityOrder) {
                err = "Line " + std::to_string(lineNo) + ": 'order' must be an integer 0..8.";
                return false;
            }
            if (std::any_of(bandSeen.begin(), bandSeen.end(), [](bool v) { return v; })) {
                err = "Line " + std::to_string(lineNo) + ": 'order' must appear before 'band' lines.";
                return false;
            }
            p.order = ord;
        } else if (key == "bands") {
            constexpr int kExpected[kDirectivityBands] = {125, 250, 500, 1000, 2000, 4000, 8000};
            for (int b = 0; b < kDirectivityBands; ++b) {
                int hz = 0;
                if (!(ls >> hz) || hz != kExpected[b]) {
                    err = "Line " + std::to_string(lineNo) + ": 'bands' must be '125 250 500 1000 2000 4000 8000'.";
                    return false;
                }
            }
            std::string extra;
            if (ls >> extra) {
                err = "Line " + std::to_string(lineNo) + ": unexpected extra token in 'bands'.";
                return false;
            }
            seenBands = true;
        } else if (key == "band") {
            if (p.order < 0) {
                err = "Line " + std::to_string(lineNo) + ": 'order' must be specified before 'band'.";
                return false;
            }
            int bandId = -1;
            if (!(ls >> bandId)) {
                err = "Line " + std::to_string(lineNo) + ": missing band index or frequency.";
                return false;
            }
            constexpr int kHz[kDirectivityBands] = {125, 250, 500, 1000, 2000, 4000, 8000};
            int bIdx = (bandId >= 0 && bandId < kDirectivityBands) ? bandId : -1;
            if (bIdx < 0) {
                for (int i = 0; i < kDirectivityBands; ++i)
                    if (bandId == kHz[i]) bIdx = i;
            }
            if (bIdx < 0) {
                err = "Line " + std::to_string(lineNo) + ": unknown band '" + std::to_string(bandId) + "'.";
                return false;
            }
            if (bandSeen[(size_t)bIdx]) {
                err = "Line " + std::to_string(lineNo) + ": duplicate band " + std::to_string(bIdx) + ".";
                return false;
            }
            const int nCoeffs = directivityNumCoeffs(p.order);
            std::vector<float> vals((size_t)nCoeffs, 0.0f);
            for (int k = 0; k < nCoeffs; ++k) {
                double v = 0.0;
                if (!(ls >> v) || !std::isfinite(v) || std::fabs(v) > 1e6) {
                    err = "Line " + std::to_string(lineNo) + ": band " + std::to_string(bIdx) +
                          " requires " + std::to_string(nCoeffs) + " finite coefficients.";
                    return false;
                }
                vals[(size_t)k] = (float)v;
            }
            std::string extra;
            if (ls >> extra) {
                err = "Line " + std::to_string(lineNo) + ": too many coefficients for order " + std::to_string(p.order) + ".";
                return false;
            }
            p.coeffs[(size_t)bIdx] = std::move(vals);
            bandSeen[(size_t)bIdx] = true;
        } else {
            err = "Line " + std::to_string(lineNo) + ": unknown directive '" + key + "'.";
            return false;
        }
    }

    if (!seenHeader) {
        err = "Missing 'VERBDIR 1' header.";
        return false;
    }
    if (p.id.empty()) {
        err = "Missing required 'id' field.";
        return false;
    }
    if (p.name.empty()) {
        err = "Missing required 'name' field.";
        return false;
    }
    if (!seenBands) {
        err = "Missing required 'bands 125 250 500 1000 2000 4000 8000' line.";
        return false;
    }
    for (int b = 0; b < kDirectivityBands; ++b) {
        if (!bandSeen[(size_t)b]) {
            err = "Missing coefficients for band " + std::to_string(b) + ".";
            return false;
        }
    }
    std::string normErr;
    if (!normalizeProfilePower(p, &normErr)) {
        err = normErr;
        return false;
    }
    out = std::move(p);
    return true;
}

inline std::string writeVerbdir(const DirectivityProfile& p) {
    auto clean = [](std::string s) {
        for (char& c : s) if (c == '\r' || c == '\n') c = ' ';
        return detail::trimStr(s);
    };
    std::ostringstream out;
    out << "VERBDIR 1\n";
    out << "id " << (detail::validProfileId(p.id) ? p.id : "custom") << "\n";
    out << "name " << (p.name.empty() ? "Custom" : clean(p.name)) << "\n";
    if (!p.category.empty()) out << "category " << clean(p.category) << "\n";
    if (!p.license.empty()) out << "license " << clean(p.license) << "\n";
    if (!p.attribution.empty()) out << "attribution " << clean(p.attribution) << "\n";
    if (p.isModel) out << "model 1\n";
    out << "order " << p.order << "\n";
    out << "bands 125 250 500 1000 2000 4000 8000\n";
    out << std::fixed << std::setprecision(6);
    constexpr int kHz[kDirectivityBands] = {125, 250, 500, 1000, 2000, 4000, 8000};
    const int nCoeffs = directivityNumCoeffs(p.order);
    for (int b = 0; b < kDirectivityBands; ++b) {
        out << "band " << kHz[b];
        const auto& c = p.coeffs[(size_t)b];
        for (int k = 0; k < nCoeffs; ++k) {
            const float v = k < (int)c.size() ? c[(size_t)k] : 0.0f;
            out << " " << v;
        }
        out << "\n";
    }
    return out.str();
}

// Transform a world unit vector (dx, dy, dz) into the transducer's local frame (+x forward, +y left, +z up)
// defined by azimuthDeg (CCW from +x) and elevationDeg (pitch up from horizontal plane).
inline void worldToLocalDir(double dx, double dy, double dz, double azimuthDeg, double elevationDeg,
                            double& ux, double& uy, double& uz) {
    const double a = azimuthDeg * (kDirectivityPi / 180.0);
    const double ca = std::cos(a), sa = std::sin(a);
    if (elevationDeg == 0.0) {
        ux = dx * ca + dy * sa;
        uy = -dx * sa + dy * ca;
        uz = dz;
    } else {
        const double e = elevationDeg * (kDirectivityPi / 180.0);
        const double ce = std::cos(e), se = std::sin(e);
        ux = dx * (ce * ca) + dy * (ce * sa) + dz * se;
        uy = -dx * sa + dy * ca;
        uz = -dx * (se * ca) - dy * (se * sa) + dz * ce;
    }
    const double r2 = ux * ux + uy * uy + uz * uz;
    if (r2 > 1e-24 && std::fabs(r2 - 1.0) > 1e-6) {
        const double invR = 1.0 / std::sqrt(r2);
        ux *= invR;
        uy *= invR;
        uz *= invR;
    }
}

#if __has_include("directivity_builtins.inc")
#include "directivity_builtins.inc"
#define VERB_HAS_MEASURED_DIRECTIVITY_BUILTINS 1
#else
#define VERB_HAS_MEASURED_DIRECTIVITY_BUILTINS 0
#endif

// Registry of built-in directivity profiles (analytic models + measured CC BY 4.0 datasets).
inline const std::vector<std::shared_ptr<const DirectivityProfile>>& builtinDirectivities() {
    static const std::vector<std::shared_ptr<const DirectivityProfile>> list = []() {
        std::vector<std::shared_ptr<const DirectivityProfile>> v;
#if VERB_HAS_MEASURED_DIRECTIVITY_BUILTINS
        appendMeasuredBuiltinDirectivities(v);
#endif
        // First-principles analytic models written by us, explicitly marked (model) per R3/brief:
        // 1. Mouth on a rigid sphere (voice: head radius 0.0875 m, mouth radius 0.015 m)
        v.push_back(std::make_shared<const DirectivityProfile>(buildSphereCapDirectivity(
            "voice_mouth_sphere_model",
            "Voice — mouth on a sphere (model)",
            "Voice",
            0.0875,
            0.015,
            "Analytic model: vibrating circular mouth aperture (r = 1.5 cm) on a rigid spherical head (R = 8.75 cm), Morse & Ingard §7.2.")));
        // 2. Piston in a sphere (brass bells, parameterised by bell radius a):
        v.push_back(std::make_shared<const DirectivityProfile>(buildSphereCapDirectivity(
            "brass_trumpet_bell_model",
            "Brass — trumpet bell, r = 6.2 cm (model)",
            "Brass",
            0.062 / 0.76604444,
            0.062,
            "Analytic model: piston in a rigid sphere (bell radius a = 6.2 cm), Morse & Ingard §7.2.")));
        v.push_back(std::make_shared<const DirectivityProfile>(buildSphereCapDirectivity(
            "brass_trombone_bell_model",
            "Brass — trombone bell, r = 10.8 cm (model)",
            "Brass",
            0.108 / 0.76604444,
            0.108,
            "Analytic model: piston in a rigid sphere (bell radius a = 10.8 cm), Morse & Ingard §7.2.")));
        v.push_back(std::make_shared<const DirectivityProfile>(buildSphereCapDirectivity(
            "brass_horn_bell_model",
            "Brass — horn bell, r = 15.0 cm (model)",
            "Brass",
            0.150 / 0.76604444,
            0.150,
            "Analytic model: piston in a rigid sphere (bell radius a = 15.0 cm), Morse & Ingard §7.2.")));
        v.push_back(std::make_shared<const DirectivityProfile>(buildSphereCapDirectivity(
            "brass_tuba_bell_model",
            "Brass — tuba bell, r = 20.0 cm (model)",
            "Brass",
            0.200 / 0.76604444,
            0.200,
            "Analytic model: piston in a rigid sphere (bell radius a = 20.0 cm), Morse & Ingard §7.2.")));
        return v;
    }();
    return list;
}

inline std::shared_ptr<const DirectivityProfile> findBuiltinDirectivity(const std::string& id) {
    if (id.empty() || id == "none") return nullptr;
    for (const auto& p : builtinDirectivities())
        if (p && p->id == id) return p;
    return nullptr;
}

}  // namespace verb

