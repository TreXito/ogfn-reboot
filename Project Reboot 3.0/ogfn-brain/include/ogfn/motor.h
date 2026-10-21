#pragma once
#include "brain.h"
#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// Gun classes the motor distinguishes. Deliberately coarse: three or four feel
// groups is what a player actually perceives, and more would be false precision.
enum class WeaponClass { Rifle, Smg, Shotgun, Sniper };

// What the bot is holding, in the terms the motor actually needs.
//
// A gun is not one number. Mag size and reload time turn "holds the trigger
// forever" into "runs dry, ducks, reloads", but the CLASS is what a player hears:
// a shotgun that fires as fast as a rifle is not a shotgun, and one trigger curve
// for every gun is how a lobby of bots ends up sounding identical.
//
// The most important field here is shot_interval_secs. Before it existed the motor
// fired on every tick the trigger was down, which is 60 shots a second at a 60 Hz
// tick: 3600 RPM. No weapon in any shooter does that, and it made every weapon
// sound like the same hose.
struct WeaponProfile {
  int mag_size = 30;
  float reload_secs = 2.6f;
  WeaponClass cls = WeaponClass::Rifle;

  // Cyclic rate: the floor on time between shots. Real-ish for the class.
  float shot_interval_secs = 0.1f;

  // Trigger discipline shape, in SECONDS of hold. A shotgun and a sniper are
  // single_shot: the hand comes off the trigger after each round no matter what,
  // because holding it does nothing.
  float burst_min_secs = 0.16f;
  float burst_max_secs = 0.44f;
  bool single_shot = false;

  // How tight the crosshair must be before the class will take the shot, as a
  // multiple of the bot's own tolerance. A sniper missing by 3 degrees misses.
  float tolerance_scale = 1.f;

  // Muzzle climb per shot, degrees. Big on a shotgun, small on a laser.
  float recoil_per_shot = 0.15f;

  // Beyond this the class is wasting ammo, so it taps instead of holding.
  float tap_range_m = 45.f;

  WeaponProfile() = default;
  WeaponProfile(int mag, float reload) : mag_size(mag), reload_secs(reload) {}
};

// The three-and-a-half guns that matter, with plausible numbers. Not real weapon
// names: this library is engine- and IP-agnostic.
WeaponProfile weapon_rifle();
WeaponProfile weapon_smg();
WeaponProfile weapon_shotgun();
WeaponProfile weapon_sniper();
WeaponProfile weapon_for_class(WeaponClass c);

// Kinematic state of the thing being shot at, as the motor needs it. Without
// velocity there is no lead, and a bot that never leads is the single clearest
// aimbot signature there is: it always shoots where you WERE.
struct TargetMotion {
  Vec3 velocity;        // m/s, horizontal is what matters
  Vec3 head_offset{0.f, 0.f, 0.55f};  // chest, relative to the reported position
  bool valid = false;
  TargetMotion() = default;
  TargetMotion(Vec3 v) : velocity(v), valid(true) {}
};

// What the motor tells the engine to do this tick. Kept abstract so the same
// motor can drive a real game client or a headless sim.
struct MotorOutput {
  float yaw_deg = 0.f;
  float pitch_deg = 0.f;
  float move_yaw_deg = 0.f;
  bool moving = false;
  // The trigger is HELD this tick. This is what an engine needs: a weapon fires
  // while the button is down, at its own cyclic rate.
  bool firing = false;
  // A round actually left the barrel this tick, per the brain's own cyclic-rate
  // model. Ammo, recoil and every telemetry number count these, not held frames:
  // conflating the two is how "3600 RPM" hid for so long.
  bool shot_fired = false;
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
    shot_cooldown_ = 0.f;
    in_burst_ = false;
    gap_left_ = 0.f;
  }
  const WeaponProfile& weapon() const { return weapon_; }

  // Magazine state. A bot that never runs dry is not carrying a gun, it is
  // carrying a hose.
  int rounds_left() const { return rounds_left_; }
  // Seconds until the gun can fire again, per its cyclic rate.
  float shot_cooldown() const { return shot_cooldown_; }
  bool reloading() const { return reloading_; }
  int reload_count() const { return reload_count_; }
  void begin_reload();

  // Advance time-based state (reload, burst phase) without aiming. The engine
  // calls this every frame; a reload must keep running while the bot is looking
  // somewhere else, otherwise it gets stuck mid-reload forever, which showed up
  // in the bench as a bot that was reloading 75% of the match.
  void tick(float dt);

  // Simulate one tick of the human-ish aiming pipeline toward a target.
  //
  // `current_pitch` is passed by value and the corrected pitch comes back on the
  // output: the motor is the only thing that knows about vertical aim, and it used
  // to never write pitch at all, so every bot stared dead level for a whole match.
  // `motion` is optional; without it the bot cannot lead, which is a visible tell,
  // so a caller that has velocity should always pass it.
  MotorOutput aim_at(const Vec3& self_pos, float current_yaw, float current_pitch,
                     const Vec3& target_pos, bool fire, float dt,
                     const TargetMotion& motion = TargetMotion());

  // Convenience for callers with no vertical state and no velocity (tests, tools).
  MotorOutput aim_at(const Vec3& self_pos, float current_yaw, const Vec3& target_pos,
                     bool fire, float dt) {
    return aim_at(self_pos, current_yaw, 0.f, target_pos, fire, dt, TargetMotion());
  }

  // Ticks (at 1/60) a flick from `from_deg` off target takes to land. Used by
  // tests to pin the turn rate inside the human range.
  int flick_ticks_to_land(const Vec3& self_pos, float start_yaw, const Vec3& target_pos);

  // Idle scanning: humans look around, they do not stare down the axis. Vertical
  // drift is included because a player's view is not locked to the horizon.
  MotorOutput idle_look(float current_yaw, float current_pitch, float dt);
  MotorOutput idle_look(float current_yaw, float dt) { return idle_look(current_yaw, 0.f, dt); }

  // Build keypresses for this tick given how fast this bot actually plays.
  int build_inputs_this_tick(float dt);

  // Returns true when the flinch delay has passed and the bot is allowed to
  // react to a fresh damage event.
  bool damage_reacts(double now, float damage);

  float aim_error_deg() const { return last_aim_error_deg_; }
  float pitch_error_deg() const { return last_pitch_error_deg_; }

  // Trigger discipline: humans tap-fire at long range and hold the trigger up
  // close. Returns whether the trigger is down this tick. Distance-aware, so it
  // is called from aim_at which already knows the range.
  bool trigger_discipline(float distance, float dt);

 private:
  Persona persona_;
  Rng rng_;
  float yaw_velocity_ = 0.f;      // deg/s, for the servo
  float pitch_velocity_ = 0.f;
  // Estimated angular velocity of the thing being tracked, for feed-forward. Without
  // it the servo lags a moving target by an amount proportional to its speed.
  float track_vel_ = 0.f;
  float track_settle_ = 0.f;   // seconds on target within the tracking band
  float last_aim_error_deg_ = 0.f;
  float last_pitch_error_deg_ = 0.f;
  float recoil_pitch_ = 0.f;      // accumulated muzzle climb, decays between bursts
  float settle_ = 0.f;            // 0..1, how settled the current track is
  float idle_pitch_target_ = 0.f; // where the idle view wants to be vertically
  float jitter_phase_ = 0.f;
  float idle_scan_dir_ = 1.f;
  double damage_gate_until_ = -1e9;
  float build_accum_ = 0.f;
  // Bursts are holds with their own sampled duration and phase, not a duty cycle.
  bool in_burst_ = false;
  float burst_len_ = 0.3f;
  float burst_elapsed_ = 0.f;
  float gap_left_ = 0.f;
  int shots_fired_ = 0;
  float burst_hold_frac_ = 1.f;
  float burst_start_phase_ = 0.f;  // sub-tick start offset, so releases are not tick-quantised
  bool trigger_held_ = false;  // hysteresis latch: held while roughly on target

  WeaponProfile weapon_{};
  // Cyclic-rate debt. Time is banked here when the trigger is down but the gun is
  // not ready, so a burst is limited by the weapon and not by the tick rate.
  float shot_cooldown_ = 0.f;
  int rounds_left_ = 30;
  bool reloading_ = false;
  float reload_remaining_ = 0.f;
  int reload_count_ = 0;
};
}  // namespace ogfn
