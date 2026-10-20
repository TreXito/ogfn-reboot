#pragma once
#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// Engine-side facts the locomotion layer needs but the brain must not invent.
struct MoveState {
  float vertical_speed = 0.f;  // m/s, negative falling
  bool airborne = false;
  bool has_cover = false;      // a wall/ramp within arm's reach to peek around
  bool on_slope = false;       // steep ground, players slow down here
};

// What the engine gets told to do about legs this tick. Separate from MotorOutput
// so a bot can aim and strafe on independent cadences, which is what humans do:
// the mouse and the WASD keys are not one hand.
struct MoveOutput {
  float move_yaw_deg = 0.f;  // world-space heading to hold
  float speed_scale = 0.f;   // 0 = standing still, 1 = full sprint
  bool crouching = false;
  bool jumping = false;
  bool strafing = false;
  bool bracing = false;  // falling far enough that the player has stopped steering
};

// The movement tell. A bot that stands still, strafes on a fixed beat, or jumps
// at a machine cadence is obvious even with good aim, so this layer is built the
// same way as the others: a cadence sampled from a human range, per-bot habits
// that persist, and hard caps where humans demonstrably have them.
//
// Deliberately NOT modelled: bunny-hopping at frame rate, 8-directional perfect
// strafe cancelling, or crouch-spam faster than a hand can tap.
class Locomotion {
 public:
  Locomotion(const Persona& p, uint64_t seed) : persona_(p), rng_(seed) {
    // Which shoulder this bot habitually leads with, held for the whole match.
    strafe_sign_ = rng_.unit() < 0.5f ? -1.f : 1.f;
    // Most players have a favourite side and only reverse when it stops working.
    side_loyalty_ = clampf(0.45f + 0.35f * p.patience, 0.35f, 0.85f);
    // How willing this bot is to crouch-spam. Not everyone does it, and the ones
    // who do are usually the ones with a fast reaction.
    crouch_propensity_ = clampf(0.35f + 0.5f * p.aim_skill, 0.f, 0.9f);
    retime();
  }

  MoveOutput update(float dt, float self_yaw_deg, float yaw_to_target_deg,
                    float target_distance, bool engaged, bool firing, bool reloading,
                    float health_frac, const MoveState& st);

  // Telemetry hooks. The bench and the tests both read these, because "does it
  // strafe" is not a property, "does it strafe at a human rate" is.
  int strafe_flips() const { return strafe_flips_; }
  float strafe_sign() const { return strafe_sign_; }
  // Crouch tap rate, exposed so a test can assert it is a hand's rate and not a
  // frame rate.
  float crouch_tap_period() const { return crouch_frame_cadence(); }

 private:
  void retime();
  float crouch_frame_cadence() const;

  Persona persona_;
  Rng rng_;

  float strafe_sign_ = 1.f;
  float side_loyalty_ = 0.6f;
  float crouch_propensity_ = 0.5f;

  double next_flip_ = 0.0;
  double now_ = 0.0;
  int strafe_flips_ = 0;

  float crouch_phase_ = 0.f;
  float crouch_period_ = 0.18f;
  int crouch_taps_left_ = 0;
  double next_crouch_ = 0.0;
  bool crouching_ = false;
  double last_stance_change_ = -1e9;
  double next_jump_ = 0.0;
  double jump_gate_until_ = 0.0;

  bool was_airborne_ = false;
  bool landing_brace_ = false;
};
}  // namespace ogfn
