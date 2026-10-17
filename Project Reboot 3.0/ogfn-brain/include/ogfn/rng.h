#pragma once
#include <cmath>
#include <cstdint>
#include <random>

namespace ogfn {

// Every bot owns one Rng, seeded explicitly, so a whole match replays from seeds
// and statistics across a population are reproducible.
class Rng {
 public:
  explicit Rng(uint64_t seed = 0xC0FFEEull) : eng_(seed) {}

  float unit() { return std::uniform_real_distribution<float>(0.f, 1.f)(eng_); }
  float range(float a, float b) { return std::uniform_real_distribution<float>(a, b)(eng_); }
  float normal(float mean, float sd) { return std::normal_distribution<float>(mean, sd)(eng_); }
  int int_range(int a, int b) { return std::uniform_int_distribution<int>(a, b)(eng_); }

  // Human reaction times are right-skewed, so sample log-normal around a median
  // rather than symmetric noise around a mean.
  float lognormal_median(float median_s, float sigma) {
    if (median_s <= 0.f) return 0.f;
    const float mu = std::log(median_s);
    return std::lognormal_distribution<float>(mu, std::max(1e-4f, sigma))(eng_);
  }

 private:
  std::mt19937_64 eng_;
};

}  // namespace ogfn
