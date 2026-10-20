#include "ogfn/motor.h"

#include <cmath>

namespace ogfn {

void Motor::tick(float dt) {
  if (dt <= 0.f) return;
  if (reloading_) {
    reload_remaining_ -= dt;
    if (reload_remaining_ <= 0.f) {
      reloading_ = false;
      rounds_left_ = weapon_.mag_size;
    }
  }
}

MotorOutput Motor::aim_at(const Vec3& self_pos, float current_yaw, float current_pitch,
                          const Vec3& target_pos, bool fire, float dt,
                          const TargetMotion& motion) {
  MotorOutput out;
  if (dt <= 0.f) return out;

  // Reload time passes whether or not the bot is aiming this tick.
  tick(dt);

  // Aim at the upper chest, not the head: humans aim at mass.
  const Vec3 aim_point = target_pos + motion.head_offset;
  const float dist = length(aim_point - self_pos);

  // --- lead ------------------------------------------------------------------
  // A human leads by eye, badly. Two components:
  //   * the correct lead, which requires knowing the target's speed, so it is
  //     imperfect: error scales with speed, with a per-bot competence factor;
  //   * a systematic under-lead, because people lag moving targets by a beat.
  // A bot that lands exactly on a strafing target is an aimbot; a bot that never
  // leads at all is the other obvious signature. This sits between the two.
  Vec3 predicted = aim_point;
  if (motion.valid) {
    const float speed = length(motion.velocity);
    if (speed > 0.05f) {
      // Flight time, capped: nobody leads a 300m shot in a close-range fight, and
      // an unbounded lead swings wildly on a fast target.
      const float flight = clampf(dist / 120.f, 0.f, 0.25f);
      // Competence scales how MUCH of the correct lead the bot applies. The servo
      // itself lags behind wherever the crosshair is told to go, so a factor below
      // 1.0 nets out to "no lead at all" (measured: -4.2 deg behind at 1.7s). A
      // decent player leads slightly too far and corrects, so the servo lag is
      // offset rather than double-counted.
      const float competence = lerpf(0.95f, 1.45f, persona_.aim_skill);
      predicted = aim_point + motion.velocity * (flight * competence);
      // Under-lead bias: people are a little late on a fast target. Small on
      // purpose, because the servo lag already contributes most of the late-ness.
      predicted = predicted - motion.velocity * (flight * 0.10f);
    }
  }

  // --- yaw -------------------------------------------------------------------
  const float desired_yaw = yaw_between(self_pos, predicted);
  const float err = wrap180(desired_yaw - current_yaw);
  last_aim_error_deg_ = std::fabs(err);

  // Human flick profile: a velocity-limited servo, not a stiff spring. The real
  // constraint is maximum turn rate, which is why a 90 degree flick costs a
  // human about a fifth of a second and a bot costs nothing.
  const float max_rate = lerpf(240.f, 480.f, persona_.aim_skill);
  const float approach = lerpf(4.5f, 8.5f, persona_.aim_skill);
  float want_v = clampf(err * approach, -max_rate, max_rate);
  // Ease down near the target so the crosshair does not ring around it.
  if (std::fabs(err) < 12.f) want_v *= 0.55f;

  // Smoothing: the hand cannot change velocity in one frame.
  const float response = clampf(lerpf(10.f, 16.f, persona_.aim_skill) * dt, 0.f, 1.f);
  yaw_velocity_ = lerpf(yaw_velocity_, want_v, response);

  // Overshoot: a fast flick carries past the target and is corrected back. This is
  // the difference between a servo and a hand, and it was documented in this file
  // for a long time without existing in the code.
  if (std::fabs(err) > 25.f && std::fabs(yaw_velocity_) > max_rate * 0.6f) {
    // Carrying speed into a large correction: commit a little past it.
    yaw_velocity_ *= 1.f + rng_.range(0.05f, 0.22f);
  }

  // Hand jitter: the crosshair never sits dead still, and this is the single
  // biggest tell that a player is a machine.
  jitter_phase_ += dt * rng_.range(5.f, 9.f);
  const float jitter = std::sin(jitter_phase_) * lerpf(0.9f, 0.25f, persona_.aim_skill);
  // Occasional mouse slip: bigger and more frequent for the unskilled.
  if (rng_.unit() < dt * lerpf(0.5f, 0.06f, persona_.aim_skill)) {
    yaw_velocity_ += rng_.range(-1.f, 1.f) * lerpf(70.f, 15.f, persona_.aim_skill);
  }

  out.yaw_deg = wrap180(current_yaw + (yaw_velocity_ + jitter) * dt);

  // --- pitch -----------------------------------------------------------------
  // Vertical aim never existed in this motor: pitch_deg was left at zero, so every
  // bot tracked at exactly the horizon for an entire match. Now it tracks the aim
  // point vertically and carries recoil, which is the other half of why a human is
  // not pointing at your feet.
  const float desired_pitch = rad2deg(std::atan2(predicted.z - self_pos.z,
                                                 std::hypot(predicted.x - self_pos.x,
                                                            predicted.y - self_pos.y)));
  const float pitch_err = desired_pitch - current_pitch;
  last_pitch_error_deg_ = std::fabs(pitch_err);
  const float pitch_approach = lerpf(5.f, 9.f, persona_.aim_skill);
  // Compensate the climb: a player pulls down against recoil, with imperfect
  // strength that scales with skill and a fixed lag. Without this, recoil pushed
  // pitch error past tolerance and truncated every burst to ~10 ticks no matter
  // what duration was sampled, which is what made burst lengths look quantised.
  const float comp = recoil_pitch_ * lerpf(0.55f, 1.0f, persona_.aim_skill);
  float want_pv = clampf(pitch_err * pitch_approach - comp * 12.f,
                         -max_rate * 0.7f, max_rate * 0.7f);
  pitch_velocity_ = lerpf(pitch_velocity_, want_pv, response);
  // Muzzle climb while firing, then a human-style pull back down.
  out.firing = false;  // decided below; recoil updated after the trigger is resolved
  const float pitch_now = current_pitch + pitch_velocity_ * dt + recoil_pitch_;
  out.pitch_deg = wrap180(pitch_now);

  // --- trigger ---------------------------------------------------------------
  // Fire only once the crosshair is plausibly on target, and respect trigger
  // discipline: nobody holds the trigger for a whole magazine at 60 metres.
  // Both axes must be on target: a bot that fires with its crosshair at the
  // target's feet is worse than one that does not fire at all.
  const float tolerance = lerpf(6.5f, 2.0f, persona_.aim_skill);
  // Fire HYSTERESIS. Without it the trigger flickers at frame rate whenever the
  // crosshair sits near the tolerance boundary, which quantised every burst to a
  // multiple of 1/60s (9 distinct lengths in 5484 bursts). A human does not tap the
  // trigger 60 times a second: they pull it, hold while roughly on target, release
  // when clearly off. Releasing needs a wider error than acquiring.
  const float release_tol = tolerance * 1.9f;
  const bool within_acquire = last_aim_error_deg_ < tolerance &&
                              last_pitch_error_deg_ < tolerance * 1.6f;
  const bool within_hold = last_aim_error_deg_ < release_tol &&
                           last_pitch_error_deg_ < release_tol * 1.6f;
  const bool on_target = fire && (trigger_held_ ? within_hold : within_acquire);

  // Magazine. Holding the trigger must eventually run dry, and a dry gun means
  // the trigger does nothing until the reload finishes. Humans also reload in
  // the gaps instead of waiting to be caught empty, so a bot with a quiet second
  // tops up.
  // Inter-burst gap: the hand is off the trigger between bursts, and that pause is
  // part of what makes fire sound human rather than continuous.
  if (gap_left_ > 0.f) {
    gap_left_ -= dt;
  }

  bool trigger = false;
  if (reloading_) {
    trigger = false;
  } else if (on_target) {
    if (rounds_left_ <= 0) {
      begin_reload();
      trigger = false;
    } else if (gap_left_ <= 0.f && trigger_discipline(dist, dt)) {
      --rounds_left_;
      trigger = true;
      ++shots_fired_;
    }
  } else if (!fire && rounds_left_ * 2 < weapon_.mag_size && rng_.unit() < dt * 0.6f) {
    // Top up in the quiet moments, the way people do, not only when dry. The
    // half-mag threshold is the habit: someone with a full-ish gun keeps moving.
    begin_reload();
  }

  out.firing = trigger && !reloading_;
  // The trigger latch: set when the hand is on it, cleared when the burst ends.
  if (out.firing) {
    trigger_held_ = true;
  } else if (in_burst_ == false || gap_left_ > 0.f || reloading_ || !within_hold) {
    trigger_held_ = false;
  }

  // Recoil: each shot climbs the muzzle a little, and the hand pulls back down with
  // lag. Clamped so a long burst cannot point the bot at the sky.
  if (trigger) {
    recoil_pitch_ = clampf(recoil_pitch_ + lerpf(0.22f, 0.08f, persona_.aim_skill), 0.f, 4.5f);
  } else {
    recoil_pitch_ = lerpf(recoil_pitch_, 0.f, clampf(dt * 4.f, 0.f, 1.f));
  }

  out.move_yaw_deg = desired_yaw;
  out.moving = true;
  return out;
}

bool Motor::trigger_discipline(float distance, float dt) {
  // Tap at range, short bursts mid, hold it close. Attempting to one-tap at 60m
  // is a human thing to do badly, not to skip entirely, so the duty cycle is
  // never exactly zero.
  float base_duty = 1.f;
  if (distance > 45.f) base_duty = lerpf(0.10f, 0.30f, persona_.aim_skill);
  else if (distance > 18.f) base_duty = lerpf(0.35f, 0.60f, persona_.aim_skill);
  else base_duty = 1.f;   // in someone's face, magazine goes brrr

  // A burst is a HOLD with a duration, not a periodic duty cycle. The duty-cycle
  // model quantised every release to a multiple of the tick (9 distinct burst
  // lengths over 5484 bursts, 26% of them the same value): a metronome, which is
  // the most machine-like thing a weapon can sound like. Here each burst draws its
  // own duration and its own sub-tick phase offset, so no two are identical.
  if (!in_burst_) {
    in_burst_ = true;
    const float base = distance > 45.f ? rng_.range(0.10f, 0.28f)
                     : distance > 18.f ? rng_.range(0.16f, 0.44f)
                                       : rng_.range(0.30f, 1.15f);
    burst_len_ = base * lerpf(0.85f, 1.2f, persona_.patience);
    burst_hold_frac_ = distance > 45.f ? rng_.range(0.45f, 0.8f) : 1.f;
    // Sub-tick release offset. out.firing is a per-tick boolean, so a burst can only
    // END on a tick boundary; with a fixed tick that quantises every duration to a
    // multiple of 1/60s (13 distinct values over 5716 bursts, top value 20% of them).
    // Humans release the trigger wherever their finger is, so the next burst starts
    // at a random sub-tick phase and the effective durations are continuous.
    burst_start_phase_ = rng_.range(0.f, 1.f) * dt;
    // Sub-tick phase: the hand does not start the burst exactly on a frame boundary.
    burst_elapsed_ = -burst_start_phase_;
    // Occasional short stab (a tap mid-burst, a panic trigger).
    if (rng_.unit() < 0.10f) burst_len_ *= rng_.range(0.25f, 0.6f);
  }

  burst_elapsed_ += dt;
  const bool within = burst_elapsed_ < burst_len_ * burst_hold_frac_;
  if (burst_elapsed_ >= burst_len_) {
    in_burst_ = false;
    // Carry the overrun into the gap, so the NEXT burst does not start on a tick
    // boundary either. Without this every duration is a multiple of 1/60s and the
    // distribution has a handful of distinct values no matter how it is sampled.
    gap_left_ = rng_.range(0.03f, 0.22f) * lerpf(0.7f, 1.4f, persona_.patience)
                - (burst_elapsed_ - burst_len_);
    if (gap_left_ < 0.f) gap_left_ = 0.f;
  }
  (void)base_duty;
  return within;
}

void Motor::begin_reload() {
  if (reloading_) return;
  reloading_ = true;
  ++reload_count_;
  // Reload time varies a little: a person is not a metronome, and a reload that
  // is cut short by being pushed is the normal case, not the exception.
  reload_remaining_ = weapon_.reload_secs * rng_.range(0.92f, 1.15f);
}

int Motor::flick_ticks_to_land(const Vec3& self_pos, float start_yaw, const Vec3& target_pos) {
  const float dt = 1.f / 60.f;
  float yaw = start_yaw;
  for (int i = 0; i < 600; ++i) {
    const MotorOutput o = aim_at(self_pos, yaw, target_pos, false, dt);
    yaw = o.yaw_deg;
    if (aim_error_deg() < 5.f) return i;
  }
  return 600;
}

MotorOutput Motor::idle_look(float current_yaw, float current_pitch, float dt) {
  MotorOutput out;
  tick(dt);  // a reload finishes while the bot is scanning, as it should
  // Sweep, pause, sweep back. Occasionally glance 90 degrees off axis so bots
  // appear to check their flanks like a person would.
  if (rng_.unit() < dt * 0.15f) idle_scan_dir_ = -idle_scan_dir_;
  float sweep = idle_scan_dir_ * lerpf(25.f, 60.f, persona_.patience);
  const float target = wrap180(current_yaw + sweep * dt * 0.6f);
  // Vertical drift: a player's view is not welded to the horizon. Real players
  // check rooftops, look down at loot, and generally sit a few degrees off level.
  idle_pitch_target_ += rng_.normal(0.f, 0.6f) * dt * 8.f;
  idle_pitch_target_ = clampf(idle_pitch_target_, -18.f, 14.f);
  const float pitch_target = current_pitch + (idle_pitch_target_ - current_pitch) *
                                                 clampf(dt * 3.f, 0.f, 1.f);
  out.yaw_deg = target;
  out.pitch_deg = clampf(pitch_target, -85.f, 85.f);
  out.move_yaw_deg = target;
  out.moving = false;
  return out;
}

int Motor::build_inputs_this_tick(float dt) {
  // Inputs per second maps from skill: 2.0 is casual, 7.0 is a fast builder.
  const float hz = lerpf(2.0f, 7.0f, persona_.build_speed);
  build_accum_ += hz * dt;
  int n = static_cast<int>(build_accum_);
  build_accum_ -= static_cast<float>(n);
  // Mistimed inputs: skipped or doubled presses.
  if (n > 0 && rng_.unit() < lerpf(0.18f, 0.05f, persona_.build_speed)) {
    n = (rng_.unit() < 0.5f) ? 0 : n + 1;
  }
  return n;
}

bool Motor::damage_reacts(double now, float damage) {
  if (now < damage_gate_until_) return false;
  // Flinch: the bigger the hit, the faster the reaction, but never instant.
  const float base = persona_.reaction_ms / 1000.f;
  const float scaled = base * lerpf(1.3f, 0.8f, clampf(damage / 60.f, 0.f, 1.f));
  damage_gate_until_ = now + rng_.lognormal_median(scaled, persona_.reaction_sigma) * 0.6f;
  return true;
}

}  // namespace ogfn
