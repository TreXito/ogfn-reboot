#pragma once
#include <algorithm>
#include <cmath>

namespace ogfn {

// Horizontal plane is x/y, z is up.
struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
  Vec3() = default;
  Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalized(const Vec3& v) {
  const float l = length(v);
  return l > 1e-6f ? v * (1.f / l) : Vec3{};
}
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * clampf(t, 0.f, 1.f); }

constexpr float kPi = 3.14159265358979323846f;
inline float deg2rad(float d) { return d * kPi / 180.f; }
inline float rad2deg(float r) { return r * 180.f / kPi; }

inline float wrap180(float d) {
  while (d > 180.f) d -= 360.f;
  while (d < -180.f) d += 360.f;
  return d;
}

// Yaw in degrees, 0 = +x, CCW positive.
inline Vec3 yaw_to_dir(float yaw_deg) {
  const float r = deg2rad(yaw_deg);
  return {std::cos(r), std::sin(r), 0.f};
}

inline float yaw_between(const Vec3& from, const Vec3& to) {
  const float dx = to.x - from.x, dy = to.y - from.y;
  if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) return 0.f;
  return rad2deg(std::atan2(dy, dx));
}

}  // namespace ogfn
