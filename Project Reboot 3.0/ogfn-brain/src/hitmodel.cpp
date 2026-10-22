#include "ogfn/hitmodel.h"

#include <cmath>

#include "ogfn/rng.h"

namespace ogfn {

const char* to_string(HitResult r) {
  switch (r) {
    case HitResult::None: return "None";
    case HitResult::Cover: return "Cover";
    case HitResult::Body: return "Body";
    case HitResult::Limb: return "Limb";
    case HitResult::Head: return "Head";
    case HitResult::Fatal: return "Fatal";
  }
  return "?";
}

DamageSplit apply_damage(float amount, float shield, float health) {
  DamageSplit out;
  out.shield_left = shield;
  out.health_left = health;
  if (amount <= 0.f) return out;
  // Shield first, then health. This is why a 75-damage body shot into 100 shield
  // does zero to health, and a bot that models it the other way will misjudge every
  // engagement by one whole shield bar.
  const float into_shield = (std::min)(amount, out.shield_left);
  out.to_shield = into_shield;
  out.shield_left -= into_shield;
  const float rest = amount - into_shield;
  const float into_health = (std::min)(rest, out.health_left);
  out.to_health = into_health;
  out.health_left -= into_health;
  if (out.shield_left < 0.f) out.shield_left = 0.f;
  if (out.health_left < 0.f) out.health_left = 0.f;
  return out;
}

float HitModel::falloff_damage(const ShotInput& in) const {
  const float d = in.distance;
  if (d <= in.falloff_start_m) return in.damage;
  if (d >= in.falloff_end_m) return in.damage * in.min_damage_frac;
  const float t = (d - in.falloff_start_m) / (in.falloff_end_m - in.falloff_start_m);
  const float frac = lerpf(1.f, in.min_damage_frac, clampf(t, 0.f, 1.f));
  return in.damage * frac;
}

bool HitModel::can_connect(const ShotInput& in) const {
  if (in.distance <= 0.01f) return true;
  // Angular half-width of the body at this distance. A body is not a point: 0.42m
  // radius at 18m is 1.3 degrees, which is the number the trigger gate has to respect.
  const float body_deg = rad2deg(std::atan2(in.target_radius, in.distance));
  const float aim_err = in.distance < 0.01f ? 0.f
                      : std::fabs(wrap180(in.yaw_deg - yaw_between(in.from, in.target_center)));
  return aim_err <= body_deg + in.spread_deg;
}

ShotOutcome HitModel::resolve(const ShotInput& in, Rng& rng) const {
  ShotOutcome out;

  // Direction to the target, both axes. The cosine match is the single number that
  // says "was this shot pointed at the thing", and it is what the tests assert on.
  // Use the SHOT's own distance when the caller supplies one: the adapter measures it
  // from the pawn transforms, and recomputing it here from a possibly-stale aim point
  // is how falloff starts disagreeing with the engine.
  const Vec3 to = in.target_center - in.from;
  const float horiz = std::hypot(to.x, to.y);
  out.cosine_match = horiz > 0.001f
      ? clampf((std::cos(deg2rad(in.yaw_deg)) * to.x + std::sin(deg2rad(in.yaw_deg)) * to.y) / horiz,
               -1.f, 1.f)
      : 1.f;

  // A wall between muzzle and body absorbs the round. The ENGINE decides this with a
  // trace; the model never assumes line of sight, because "botted through a wall" is
  // the single most obvious cheat signature there is.
  if (!in.has_line_of_sight) {
    out.result = HitResult::Cover;
    return out;
  }
  if (horiz < 0.001f) {
    out.result = HitResult::None;
    return out;
  }

  // Angular error to the body centre, and the body's angular radius.
  const float want_yaw = yaw_between(in.from, in.target_center);
  const float yaw_err = wrap180(in.yaw_deg - want_yaw);

  // Vertical error against the capsule: chest is centre, so the body spans
  // +/- height/2 plus the radius. At 100m a 1-degree pitch error is 1.7m, which is
  // fully above the head: this term is what makes long shots hard, not damage math.
  const float want_pitch = rad2deg(std::atan2(to.z, horiz));
  const float pitch_err = in.pitch_deg - want_pitch;
  const float vert_miss = std::tan(deg2rad(pitch_err)) * horiz;

  // Spread: one draw per pellet, in a cone. A shotgun's pellets scatter independently,
  // which is exactly why it stops working past its range.
  int hits = 0;
  float worst_signed = 0.f;
  const int pellets = (std::max)(1, in.pellets);
  for (int p = 0; p < pellets; ++p) {
    // Uniform in the disc, scaled by the cone half-angle. Signed so a caller can see
    // which side the round went.
    const float r = in.spread_deg * std::sqrt(rng.unit());
    const float th = rng.range(0.f, 6.2831853f);
    const float dx = r * std::cos(th);
    const float dy = r * std::sin(th);

    // Total miss in degrees on each axis, then converted to meters at this range.
    const float miss_yaw_deg = yaw_err + dx;
    const float miss_vert_m = vert_miss + std::tan(deg2rad(dy)) * horiz;
    const float miss_lat_m = std::tan(deg2rad(miss_yaw_deg)) * horiz;

    if (p == 0) worst_signed = miss_lat_m;

    // Inside the body? The capsule is radius-wide horizontally, and height/2 +
    // radius vertically from the chest centre.
    const float half_v = in.target_height * 0.5f + in.target_radius;
    if (std::fabs(miss_lat_m) <= in.target_radius &&
        std::fabs(miss_vert_m) <= half_v) {
      ++hits;
      // Head is the top ~18% of the capsule: a real head is smaller than that, and
      // under-modelling it keeps headshots rare instead of routine.
      if (in.headshot_possible &&
          miss_vert_m > half_v * 0.64f &&
          std::fabs(miss_lat_m) <= in.target_radius * 0.45f) {
        out.result = HitResult::Head;
      } else if (out.result == HitResult::None) {
        // Legs and arms are the bottom and outer edges: reduced damage, which is why
        // tracking a body centre matters more than a lucky limb hit.
        const bool limb = std::fabs(miss_vert_m) > half_v * 0.55f ||
                          std::fabs(miss_lat_m) > in.target_radius * 0.62f;
        out.result = limb ? HitResult::Limb : HitResult::Body;
      }
    }
  }

  out.miss_distance = worst_signed;
  if (hits == 0) {
    out.result = HitResult::None;
    return out;
  }

  // Damage: per-pellet base, range falloff, hit-location multiplier, times pellets hit.
  float per = falloff_damage(in);
  if (out.result == HitResult::Head) per *= 1.9f;
  else if (out.result == HitResult::Limb) per *= 0.72f;

  const DamageSplit split = apply_damage(per * static_cast<float>(hits),
                                        in.target_shield, in.target_health);
  out.damage_on_shield = split.to_shield;
  out.damage_on_health = split.to_health;

  // Downed rather than dead: a battle royale goes health -> downed -> eliminated.
  // The distinction matters because a downed body is a revive target for its squad,
  // so a model that skips straight to dead makes the squad layer unreachable.
  if (split.health_left <= 0.f) {
    out.target_downed = true;
    out.result = HitResult::Fatal;
  }
  return out;
}

}  // namespace ogfn
