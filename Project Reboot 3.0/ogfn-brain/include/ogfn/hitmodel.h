#pragma once
#include <cstdint>
#include <vector>

#include "mathx.h"

namespace ogfn {

// Hit resolution: turning "the trigger was pulled, pointed HERE" into "what did that
// hit, and did it hurt".
//
// Why this lives in the brain and not the adapter: the 3.5 client this project runs on
// has NO ranged-weapon damage path at all. A live match fired 80,627 rounds and killed
// exactly nobody, because StartFire plays the fire event and the ability system never
// applies hitscan damage. The brain's job on the engine side is to pick a target and
// pull the trigger at the right moment; the job of THIS file is to decide what those
// rounds actually accomplished, using the same geometry the engine would have used.
//
// It is deliberately a pure function of geometry and weapon stats, with no engine
// types, so every rule below is pinned by a test. A bot that "hits" 100% of the time
// is not a bot that aims well, it is a bot whose hit model is lying, so the model is
// built out of the reasons a real shot misses: spread, falloff, cover, distance.

enum class HitResult {
  None,      // nothing in the line
  Cover,     // a wall/building absorbed it
  Body,      // hit the target, full damage
  Limb,      // hit the target, reduced damage
  Head,      // hit the target, multiplied damage
  Fatal,     // hit the target and it went down
};

const char* to_string(HitResult r);

// What a shot is fired from and at. Geometry in meters, angles in degrees.
struct ShotInput {
  Vec3 from;              // muzzle position
  Vec3 aim_dir_yaw_deg;   // NOT a yaw value: see yaw_deg below
  float yaw_deg = 0.f;    // where the crosshair points, horizontally
  float pitch_deg = 0.f;  // and vertically

  // The body being shot at, as a capsule. `center` is the chest.
  Vec3 target_center;
  float target_radius = 0.42f;    // shoulder half-width; a body is not a point
  float target_height = 1.8f;

  float distance = 0.f;           // meters, from `from` to the target
  Vec3 target_velocity;           // m/s, so a lead error can be expressed

  // The item in hand. These come from WeaponProfile at the adapter boundary.
  float spread_deg = 1.2f;        // cone half-angle at this distance
  float damage = 25.f;            // per pellet, at point blank
  int pellets = 1;                // shotgun > 1

  // Range behaviour: damage falls off linearly between these, then stops.
  float falloff_start_m = 30.f;
  float falloff_end_m = 90.f;
  float min_damage_frac = 0.35f;

  bool has_line_of_sight = true;  // decided by the ENGINE (a trace), never assumed
  float target_health = 100.f;
  float target_shield = 0.f;
  bool headshot_possible = true;
};

// Everything that happened, so the caller can log or test one number at a time.
struct ShotOutcome {
  HitResult result = HitResult::None;
  float damage_on_health = 0.f;
  float damage_on_shield = 0.f;
  bool target_died = false;
  bool target_downed = false;
  // Perpendicular miss distance in meters, signed by which side. This is what a
  // shooter would call "how far off was I", and it is what the spread model rolls.
  float miss_distance = 0.f;
  float cosine_match = 0.f;      // dot(aim, to_target): 1.0 is a perfect line
};

// The hit model. Deterministic given the RNG it is handed, so a replay of the same
// shots produces the same kills.
class HitModel {
 public:
  // Resolve one shot. `rng` supplies the spread draw; pass a seeded Rng so the same
  // shot sequence always resolves the same way.
  ShotOutcome resolve(const ShotInput& in, class Rng& rng) const;

  // Damage of one pellet after range falloff, ignoring hit location.
  float falloff_damage(const ShotInput& in) const;

  // Whether a shot aimed at `aim_yaw_deg` can physically reach the body: is the
  // angular error inside the angle the body subtends, plus spread?
  bool can_connect(const ShotInput& in) const;
};

// How damage lands on health and shield. Shield always eats first, which is the rule
// in every battle royale and the reason a 75-damage shot on a full-shield target does
// nothing to its health.
struct DamageSplit {
  float to_shield = 0.f;
  float to_health = 0.f;
  float shield_left = 0.f;
  float health_left = 0.f;
};
DamageSplit apply_damage(float amount, float shield, float health);

}  // namespace ogfn
