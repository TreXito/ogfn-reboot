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

MotorOutput Motor::aim_at(const Vec3& self_pos, float current_yaw, const Vec3& target_pos,
                          bool fire, float dt) {
  MotorOutput out;
  if (dt <= 0.f) return out;

  // Reload time passes whether or not the bot is aiming this tick.
  tick(dt);

  // Aim at the upper chest, not the head: humans aim at mass.
  Vec3 aim_point = target_pos;
  aim_point.z += 0.55f;

  const float desired_yaw = yaw_between(self_pos, aim_point);
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

  // Hand jitter: the crosshair never sits dead still, and this is the single
  // biggest tell that a player is a machine.
  jitter_phase_ += dt * rng_.range(5.f, 9.f);
  const float jitter = std::sin(jitter_phase_) * lerpf(0.9f, 0.25f, persona_.aim_skill);
  // Occasional mouse slip: bigger and more frequent for the unskilled.
  if (rng_.unit() < dt * lerpf(0.5f, 0.06f, persona_.aim_skill)) {
    yaw_velocity_ += rng_.range(-1.f, 1.f) * lerpf(70.f, 15.f, persona_.aim_skill);
  }

  out.yaw_deg = wrap180(current_yaw + (yaw_velocity_ + jitter) * dt);

  // Fire only once the crosshair is plausibly on target, and respect trigger
  // discipline: nobody holds the trigger for a whole magazine at 60 metres.
  const float tolerance = lerpf(6.5f, 2.0f, persona_.aim_skill);
  const float dist = length(target_pos - self_pos);
  bool on_target = fire && last_aim_error_deg_ < tolerance;

  // Magazine. Holding the trigger must eventually run dry, and a dry gun means
  // the trigger does nothing until the reload finishes. Humans also reload in
  // the gaps instead of waiting to be caught empty, so a bot with a quiet second
  // tops up.
  if (reloading_) {
    on_target = false;
  } else if (on_target) {
    if (rounds_left_ <= 0) {
      begin_reload();
      on_target = false;
    } else if (trigger_discipline(dist, dt)) {
      --rounds_left_;
    } else {
      on_target = false;
    }
  } else if (!fire && rounds_left_ * 2 < weapon_.mag_size && rng_.unit() < dt * 0.6f) {
    // Top up in the quiet moments, the way people do, not only when dry. The
    // half-mag threshold is the habit: someone with a full-ish gun keeps moving.
    begin_reload();
  }

  out.firing = on_target && !reloading_;
  out.move_yaw_deg = desired_yaw;
  out.moving = true;
  return out;
}

bool Motor::trigger_discipline(float distance, float dt) {
  // Tap at range, short bursts mid, hold it close. Attempting to one-tap at 60m
  // is a human thing to do badly, not to skip entirely, so the duty cycle is
  // never exactly zero.
  float duty;
  if (distance > 45.f) {
    duty = lerpf(0.10f, 0.30f, persona_.aim_skill);
    burst_cycle_ = lerpf(0.45f, 0.7f, persona_.patience);
  } else if (distance > 18.f) {
    duty = lerpf(0.35f, 0.60f, persona_.aim_skill);
    burst_cycle_ = lerpf(0.35f, 0.5f, persona_.patience);
  } else {
    duty = 1.f;  // in someone's face, magazine goes brrr
    burst_cycle_ = 0.5f;
  }

  burst_phase_ += dt;
  if (burst_phase_ > burst_cycle_) burst_phase_ -= burst_cycle_;
  // Occasional dropped trigger (weapon swap, repositioning).
  if (rng_.unit() < dt * 0.25f) burst_phase_ = burst_cycle_ * 0.95f;
  return burst_phase_ < burst_cycle_ * duty;
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

MotorOutput Motor::idle_look(float current_yaw, float dt) {
  MotorOutput out;
  tick(dt);  // a reload finishes while the bot is scanning, as it should
  // Sweep, pause, sweep back. Occasionally glance 90 degrees off axis so bots
  // appear to check their flanks like a person would.
  if (rng_.unit() < dt * 0.15f) idle_scan_dir_ = -idle_scan_dir_;
  float sweep = idle_scan_dir_ * lerpf(25.f, 60.f, persona_.patience);
  const float target = wrap180(current_yaw + sweep * dt * 0.6f);
  out.yaw_deg = target;
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
