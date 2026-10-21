#include "ogfn/locomotion.h"

#include <cmath>

namespace ogfn {

namespace {
// Human tap rates. A hand cannot reliably tap crouch faster than ~4Hz, and a
// 1/60 "crouch-spam" bot is the single most obvious movement tell in a shooter.
constexpr float kCrouchTapFloor = 0.24f;
constexpr double kJumpFloor = 0.85;

// Below this vertical speed a human commits to the landing instead of steering.
constexpr float kBraceFallSpeed = -14.f;
// Fall speed a player starts looking for something to land on.
constexpr float kFallAwareSpeed = -8.f;
}  // namespace

void Locomotion::retime() {
  // Reversal cadence while a fight is on. Log-normal around a median that scales
  // with how twitchy this bot is, so the flips are irregular rather than a
  // metronome, which is what makes a strafing bot look scripted.
  const float median = lerpf(0.42f, 0.24f, persona_.aim_skill);
  next_flip_ = now_ + rng_.lognormal_median(median, 0.30f);
}

float Locomotion::crouch_frame_cadence() const { return kCrouchTapFloor; }

MoveOutput Locomotion::update(float dt, float self_yaw_deg, float yaw_to_target_deg,
                              float target_distance, bool engaged, bool firing,
                              bool reloading, float health_frac, const MoveState& st,
                              float travel_yaw_deg, bool travel_supplied) {
  MoveOutput out;
  if (dt <= 0.f) return out;
  now_ += dt;

  // --- bracing: a human stops steering mid-fall ---------------------------------
  // This is checked before anything else because being unable to steer while
  // falling is a hard constraint, not a choice.
  if (st.vertical_speed < kBraceFallSpeed) {
    out.bracing = true;
    out.speed_scale = st.airborne ? 0.f : 0.15f;
    out.move_yaw_deg = self_yaw_deg;
    was_airborne_ = st.airborne;
    return out;
  }

  // Landing recovery. A human loses a beat on touchdown instead of accelerating
  // out of it, and jumping into a landing is what bots do that players do not.
  if (was_airborne_ && !st.airborne) {
    if (st.vertical_speed < kFallAwareSpeed) landing_brace_ = true;
    // Never jump again on the frame we land.
    jump_gate_until_ = now_ + rng_.range(0.12f, 0.35f);
  }
  was_airborne_ = st.airborne;

  if (landing_brace_) {
    // Recover for a short beat, scaled by how hard the landing was.
    out.speed_scale = lerpf(0.25f, 0.55f, persona_.build_speed);
    out.bracing = true;
    out.crouching = true;
    if (now_ > jump_gate_until_ + 0.1) landing_brace_ = false;
    out.move_yaw_deg = self_yaw_deg;
    return out;
  }

  // --- strafing ---------------------------------------------------------------
  // In a fight a human is almost never still. Out of one they walk in a straight
  // line toward wherever they are going and the flip logic is dormant.
  bool strafing = false;
  if (engaged) {
    strafing = true;
    if (now_ > next_flip_) {
      // Hold a favourite side longer, and reverse eagerly when the trade is
      // going badly: that is what a real player does when they start losing.
      const bool keep = rng_.unit() < (side_loyalty_ + (1.f - health_frac) * 0.15f);
      if (!keep) {
        strafe_sign_ = -strafe_sign_;
        ++strafe_flips_;
        // Reversing direction costs momentum: the player slows to change direction.
        turn_load_ = clampf(turn_load_ + 1.f, 0.f, 1.f);
      }
      retime();
    }
  } else if (now_ > next_flip_) {
    // Idle/rotating players drift a little rather than tracking a laser line.
    strafe_sign_ = -strafe_sign_;
    ++strafe_flips_;
    next_flip_ = now_ + rng_.range(1.6f, 3.4f);
  }

  // Heading: perpendicular to the target while fighting (that is what a strafe
  // is), straight toward the intent when not.
  if (engaged) {
    const float perp = yaw_to_target_deg + 90.f * strafe_sign_;
    out.move_yaw_deg = wrap180(perp);
  } else {
    out.move_yaw_deg = self_yaw_deg;
  }
  out.strafing = strafing;

  // --- speed --------------------------------------------------------------------
  // Shooting costs movement, but not all of it: standing still to aim is a
  // bot-like habit, and so is sprinting while firing.
  float base_speed;
  if (engaged) {
    base_speed = firing ? 0.72f : 0.95f;
    if (st.on_slope) base_speed *= 0.75f;
    if (reloading) base_speed = 0.85f;  // reposition while reloading
  } else {
    base_speed = st.on_slope ? 0.7f : 1.f;
  }
  // A badly hurt player backs off faster than they push, whatever their persona.
  if (engaged && health_frac < 0.35f) base_speed += 0.05f;

  // Speed is not a set of four constants. Measured, the layer produced 4 distinct
  // values with a 64% modal: a hard state machine, which is exactly the signature a
  // movement detector looks for. A human's stick input drifts continuously, and it
  // dips when they are turning or unsure where to go.
  //
  // Two continuous components:
  //   * a slow wander, so a bot's pace drifts over a couple of seconds;
  //   * a dip proportional to how hard it is currently turning, because you slow
  //     down to change direction.
  speed_wander_ += rng_.normal(0.f, 1.f) * dt * 0.55f;
  speed_wander_ = clampf(speed_wander_, -0.12f, 0.10f);
  const float turn_dip = clampf(turn_load_, 0.f, 1.f) * 0.18f;
  out.speed_scale = clampf(base_speed + speed_wander_ - turn_dip, 0.05f, 1.f);
  // Turn load decays: it is a response to a recent sharp change, not a permanent tax.
  turn_load_ = (std::max)(0.f, turn_load_ - dt * 2.2f);

  // --- crouching ----------------------------------------------------------------
  // Two habits: holding cover while not shooting, and crouch-spam while trading
  // shots at range. Both are rate limited to a hand's tapping speed, because a
  // frame-rate crouch-spam is the loudest movement tell there is. The gate is
  // global on purpose: a strobing trigger input must not strobe the stance too.
  bool want_crouch = false;
  if (engaged && st.has_cover && !firing) {
    // Peek cycle: hold cover, pop out to shoot, duck back.
    want_crouch = true;
    out.speed_scale *= 0.35f;
  } else if (engaged && firing && target_distance > 22.f && now_ > next_crouch_ &&
             rng_.unit() < crouch_propensity_) {
    // Not every player crouch-spams: the ones who do started doing it because
    // their aim is twitchy. Bots that never do it look stiff, bots that all do
    // it look like one player copied 20 times.
    // Start a crouch cycle: two to four taps, then a normal beat of standing.
    crouch_taps_left_ = rng_.int_range(2, 4);
    crouch_period_ = crouch_frame_cadence();
    crouch_phase_ = 0.f;
    next_crouch_ = now_ + rng_.range(0.9f, 2.6f);
  }

  if (crouch_taps_left_ > 0) {
    crouch_phase_ += dt;
    if (crouch_phase_ > crouch_period_) {
      crouch_phase_ -= crouch_period_;
      --crouch_taps_left_;
    }
    want_crouch = want_crouch || crouch_phase_ < crouch_period_ * 0.5f;
  }

  // Stance hysteresis: no stance change faster than a hand can tap a key.
  if (want_crouch != crouching_ && now_ - last_stance_change_ >= crouch_frame_cadence()) {
    crouching_ = want_crouch;
    last_stance_change_ = now_;
  }
  out.crouching = crouching_;

  // --- jumping -----------------------------------------------------------------
  // Humans jump to cross a gap, to dodge a third party's line, or to get to high
  // ground. They do not jump on a beat, and they cannot jump while airborne.
  if (!st.airborne && now_ > jump_gate_until_ && now_ > next_jump_) {
    bool want_jump = false;
    if (!engaged && st.on_slope) want_jump = rng_.unit() < 0.35f;      // scramble up
    // Bail out of a lost fight. Keyed on "hurt and someone close" rather than on
    // `engaged`, because the moment you decide to RUN is exactly when a player
    // jumps to break the line: the decision becomes Flee, engaged goes false, and a
    // hurt fleeing bot used to stop jumping entirely.
    if (health_frac < 0.4f && target_distance < 20.f) want_jump = rng_.unit() < 0.30f;
    if (want_jump) {
      out.jumping = true;
      next_jump_ = now_ + kJumpFloor + rng_.range(0.f, 1.4f);
      jump_gate_until_ = now_ + kJumpFloor;
    }
  }

  // --- gaze ---------------------------------------------------------------------
  // A player looks where they run. Out of combat the view belongs to the travel
  // direction, with a sideways shoulder-check every few seconds. In combat the view
  // belongs to the fight and travel is handled by strafing, so the weight drops to
  // zero: this is the part that keeps a bot from staring at a wall while walking.
  if (engaged) {
    out.gaze_weight = 0.f;
    out.gaze_yaw_deg = yaw_to_target_deg;
  } else if (out.bracing) {
    // Falling: no interest in looking anywhere in particular.
    out.gaze_weight = 0.f;
    out.gaze_yaw_deg = travel_yaw_deg;
  } else {
    // Shoulder-check: hold a glance off-axis for a beat, then return to travel.
    // With nowhere to go there is nothing to check relative to: the view holds,
    // which is what standing still actually looks like. Without this the bot
    // pirouetted in place while idle (a glance relative to an arbitrary heading),
    // and the body rotation is also the perception FOV axis.
    if (!travel_supplied) {
      out.gaze_weight = 0.f;
      out.gaze_yaw_deg = self_yaw_deg;
      return out;
    }
    if (now_ > next_glance_) {
      // A shoulder check is a glance to ONE flank, 25 to 55 degrees, not a spin. The
      // first version allowed +-70 and a 10% p90 of 102 degrees off travel, which is
      // a bot pirouetting while it runs.
      glance_offset_ = rng_.range(25.f, 55.f) * (rng_.unit() < 0.5f ? -1.f : 1.f);
      glance_until_ = now_ + rng_.range(0.35f, 0.9f);
      next_glance_ = now_ + rng_.range(3.5f, 9.0f);
    }
    const bool glancing = now_ < glance_until_;
    // Weight below 1.0 so the engine can blend the gaze in rather than snapping the
    // view, which is what the smoothing is for: a neck turns over ~0.2s.
    out.gaze_yaw_deg = glancing ? wrap180(travel_yaw_deg + glance_offset_) : travel_yaw_deg;
    out.gaze_weight = 0.85f;
  }

  return out;
}

}  // namespace ogfn
