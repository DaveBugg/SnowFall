// Small deterministic PRNG (xoshiro256** seeded through splitmix64). The simulation never
// uses <random> distributions: their output differs between standard libraries, and tests
// rely on "same seed + same call sequence => same state".
#pragma once
#include <cstdint>

namespace snow {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x5EED5EEDull) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        for (auto& v : s_) {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, 1) with 24 bits of precision (exactly representable in float).
    float uniform() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
    // Uniform integer in [0, n). n must be > 0.
    std::uint32_t below(std::uint32_t n) {
        return static_cast<std::uint32_t>(((next() >> 32) * static_cast<std::uint64_t>(n)) >> 32);
    }
    bool chance(float p) { return uniform() < p; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4]{};
};

}  // namespace snow
