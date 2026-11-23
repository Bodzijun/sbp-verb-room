#pragma once
#include <array>
#include <cstddef>

namespace verbp {

inline constexpr std::array<const char*, 3> ambisonicsNormalizationNames{{"SN3D", "N3D", "FuMa"}};

// Output slot -> ACN (WXYZ RSTUV KLMNOPQ).
// FuMa gain ratios below are in ACN order. See EP2636036B1, table 8.
struct AmbisonicsOutput {
    static constexpr int encode(int normalization, int levelDb) { return normalization * 3 - levelDb / 3; }

    explicit AmbisonicsOutput(int settings) : normalization(settings / 3), level(levels[(size_t)(settings % 3)]) {}
    bool reorders() const { return normalization == 2; }
    int acn(int output) const { return reorders() ? fumaToAcn[(size_t)output] : output; }
    float gain(int output) const {
        const size_t index = (size_t)acn(output);
        if (normalization == 2) return fuma[index] * level;
        if (normalization == 1) return n3d[index] * level;
        return level;
    }

private:
    static constexpr std::array<int, 16> fumaToAcn{{0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9}};
    static constexpr float w = 0.70710678118654752440f; // 1 / sqrt(2)
    static constexpr float f2 = 1.1547005383792515290f; // 2 / sqrt(3)
    static constexpr float f31 = 1.1858541225631422495f; // sqrt(45 / 32)
    static constexpr float f32 = 1.3416407864998738178f; // 3 / sqrt(5)
    static constexpr float f33 = 1.2649110640673517328f; // sqrt(8 / 5)
    static constexpr std::array<float, 16> fuma{{w, 1, 1, 1, f2, f2, 1, f2, f2, f33, f32, f31, 1, f31, f32, f33}};
    static constexpr float n1 = 1.7320508075688772935f; // sqrt(3)
    static constexpr float n2 = 2.2360679774997896964f; // sqrt(5)
    static constexpr float n3 = 2.6457513110645905905f; // sqrt(7)
    static constexpr std::array<float, 16> n3d{{1, n1, n1, n1, n2, n2, n2, n2, n2, n3, n3, n3, n3, n3, n3, n3}};
    static constexpr std::array<float, 3> levels{{1.f, 0.70794578438413791080f, 0.50118723362727228500f}}; // 10^(dB/20)
    int normalization;
    float level;
};

} // namespace verbp
