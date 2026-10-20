#pragma once
#include "brain.h"
#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// The gun the bot is holding, in the terms the motor actually needs. Mag size and
// reload time are what turn "holds the trigger forever" into "runs dry, ducks,
// reloads", which is a tell you can hear across the whole island.
struct WeaponProfile {
  int mag_size = 30;
  float reload_secs = 2.6f;
  WeaponProfile() = default;
  WeaponProfile(int mag, float reload) : mag_size(mag), reload_secs(reload) {}
};

// What the motor tells the engine to do this tick. Kept abstract so the same
// motor can drive a real game client or a headless sim.
struct MotorOutput {
  float yaw_deg = 0.f;
  float pitch_deg = 0.f;
  float move_yaw_deg = 0.f;
  bool moving = false;
  bool firing = false;
  bool jumping = false;
  bool crouching = false;
  float build_input_hz = 0.f;
};

// The motor is where "feels human" actually lives. Perfect aim under a perfect
// brain is still an obvious bot, so every output is degraded on purpose:
//   - aim is a servo with deadline, not a snap: overshoot then correction
//   - target tracking jitters, it never sits exactly on the head
//   - build cadence is capped and occasionally mistimed
//   - reaction to damage has a flinch before anything happens
class Motor {
 public:
  Motor(const Persona& p, uint64_t seed) : persona_(p), rng_(seed) { rounds_left_ = weapon_.mag_size; }

  void set_weapon(const WeaponProfile& w) {
    weapon_ = w;
    rounds_left_ = w.mag_size;
    reloading_ = false;
    reload_remaining_ = 0.f;
  }
  const WeaponProfile& weapon() const { return weapon_; }

  // Magazine state. A bot that never runs dry is not carrying a gun, it is
  // carrying a hose.
  int rounds_left() const { return rounds_left_; }
  bool reloading() const { return reloading_; }
  int reload_count() const { return reload_count_; }
  void begin_reload();

  // Advance time-based state (reload, burst phase) without aiming. The engine
  // calls this every frame; a reload must keep running while the bot is looking
  // somewhere else, otherwise it gets stuck mid-reload forever, which showed up
  // in the bench as a bot that was reloading 75% of the match.
  void tick(float dt);

  // Simulate one tick of the human-ish aiming pipeline toward a target.
  MotorOutput aim_at(const Vec3& self_pos, float current_yaw, const Vec3& target_pos,
                     bool fire, float dt);

  // Ticks (at 1/60) a flick from `from_deg` off target takes to land. Used by
  // tests to pin the turn rate inside the human range.
  int flick_ticks_to_land(const Vec3& self_pos, float start_yaw, const Vec3& target_pos);

  // Idle scanning: humans look around, they do not stare down the axis.
  MotorOutput idle_look(float current_yaw, float dt);

  // Build keypresses for this tick given how fast this bot actually plays.
  int build_inputs_this_tick(float dt);

  // Returns true when the flinch delay has passed and the bot is allowed to
  // react to a fresh damage event.
  bool damage_reacts(double now, float damage);

  float aim_error_deg() const { return last_aim_error_deg_; }

  // Trigger discipline: humans tap-fire at long range and hold the trigger up
  // close. Returns whether the trigger is down this tick. Distance-aware, so it
  // is called from aim_at which already knows the range.
  bool trigger_discipline(float distance, float dt);

 private:
  Persona persona_;
  Rng rng_;
  float yaw_velocity_ = 0.f;      // deg/s, for the servo
  float last_aim_error_deg_ = 0.f;
  float jitter_phase_ = 0.f;
  float idle_scan_dir_ = 1.f;
  double damage_gate_until_ = -1e9;
  float build_accum_ = 0.f;
  float burst_phase_ = 0.f;
  float burst_cycle_ = 0.4f;

  WeaponProfile weapon_{};
  int rounds_left_ = 30;
  bool reloading_ = false;
  float reload_remaining_ = 0.f;
  int reload_count_ = 0;
};
}  // namespace ogfn
