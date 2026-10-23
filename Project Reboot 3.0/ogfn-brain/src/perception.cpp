#include "ogfn/perception.h"

#include <cmath>

namespace ogfn {

namespace {
bool in_fov(const Vec3& self_pos, float self_yaw_deg, const Vec3& other, float fov_half_deg) {
  const Vec3 to = other - self_pos;
  const float dist = length(to);
  if (dist < 1e-4f) return true;
  const Vec3 fwd = yaw_to_dir(self_yaw_deg);
  const float cos_angle = dot(normalized(to), fwd);
  const float cos_limit = std::cos(deg2rad(fov_half_deg));
  return cos_angle >= cos_limit;
}

TrackedTarget* find(std::vector<TrackedTarget>& v, int id) {
  for (auto& t : v)
    if (t.id == id) return &t;
  return nullptr;
}
}  // namespace

void Perception::update(const Observation& obs, double now) {
  const float dt = clampf(static_cast<float>(now - last_time_), 0.f, cfg_.max_dt);
  last_time_ = now;
  last_self_pos_ = obs.self_pos;
  for (auto& t : targets_) t.visible = false;

  for (const auto& e : obs.enemies) {
    // Teammates are seen but never tracked as targets. Dropping them here rather
    // than at the aiming call means no downstream consumer can accidentally treat
    // one as fightable, which is the failure mode that makes a squad bot kill its
    // own squad.
    if (e.is_teammate) continue;
    const float dist = length(e.pos - obs.self_pos);
    // FOV does NOT apply to someone who just shot you. Being hit registers regardless
    // of where you are looking: that is how a player knows to spin around, and gating it
    // on the view cone is why a bot walked calmly away from fire coming from behind.
    // They still have to be within weapon range and have a real line to you.
    const bool hurt_me = e.damaged_me && dist <= cfg_.max_range && e.line_of_sight;
    const bool sensed = hurt_me ||
                        (e.line_of_sight && dist <= cfg_.max_range &&
                         in_fov(obs.self_pos, obs.self_yaw_deg, e.pos, cfg_.fov_half_deg));
    if (!sensed) continue;

    TrackedTarget* t = find(targets_, e.id);
    if (!t) {
      TrackedTarget fresh;
      fresh.id = e.id;
      targets_.push_back(fresh);
      t = &targets_.back();
    }
    t->last_seen_pos = e.pos;
    t->health = e.health;
    if (e.damaged_me) t->last_damaged_me_time = now;
    t->last_seen_time = now;
    t->visible = true;
  }

  for (auto& t : targets_) {
    if (t.visible) {
      t.tracked_for += dt;
    } else {
      t.tracked_for *= 0.5f;  // re-acquisition is not instant after losing sight
    }
    t.fightable = t.tracked_for >= cfg_.lock_secs && t.visible;
  }

  // Forget what has been out of mind too long.
  std::vector<TrackedTarget> kept;
  kept.reserve(targets_.size());
  for (const auto& t : targets_) {
    if (t.visible || t.age(now) <= cfg_.memory_secs) kept.push_back(t);
  }
  targets_.swap(kept);
}

const TrackedTarget* Perception::nearest(bool require_visible) const {
  // An aggressor outranks a closer bystander. If someone is shooting you, they are the
  // target that matters, and picking the nearer body instead means the bot trades shots
  // with whoever happens to be adjacent while the actual threat keeps firing freely.
  // last_time_ is the clock update() last saw, so this needs no extra parameter.
  for (const auto& t : targets_) {
    if (require_visible && !t.visible) continue;
    if (t.damaged_me_recently(last_time_)) return &t;
  }
  const TrackedTarget* best = nullptr;
  float best_dist = 1e9f;
  for (const auto& t : targets_) {
    if (require_visible && !t.visible) continue;
    // Distance to SELF: length(pos) is distance to world origin, which coincides
    // only while self sits at (0,0,0) -- true in every test, false for every live
    // bot (that bug made nearest() rank targets by distance-to-origin and the
    // servo chase bears it could never settle on).
    const float d = length(t.last_seen_pos - last_self_pos_);
    if (d < best_dist) {
      best_dist = d;
      best = &t;
    }
  }
  return best;
}

const TrackedTarget* Perception::by_id(int id) const {
  for (const auto& t : targets_)
    if (t.id == id) return &t;
  return nullptr;
}

bool Perception::can_see(int id, double now, float max_age) const {
  const TrackedTarget* t = by_id(id);
  if (!t) return false;
  return t->visible || t->age(now) <= max_age;
}

}  // namespace ogfn
