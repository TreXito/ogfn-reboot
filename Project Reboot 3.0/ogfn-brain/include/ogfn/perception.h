#pragma once
#include <vector>

#include "mathx.h"

namespace ogfn {

// Ground truth handed to the bot by the engine every tick. The bot is not allowed
// to use most of it: Perception throws away everything a human could not see.
struct EnemySample {
  int id = 0;
  Vec3 pos;
  float health = 100.f;
  bool line_of_sight = false;
  // This enemy damaged us since the last observation. The adapter sets it from the
  // engine's damage events; a shooter you cannot see is the whole reason a player turns
  // around. Without it a bot walks calmly away from someone shooting it in the back.
  bool damaged_me = false;
  // A teammate is not an enemy. Without this flag a bot in a squad playlist would
  // track, lock onto and shoot the person standing next to it, which is the single
  // worst tell in the game (and in the current adapter, bots do share teams).
  bool is_teammate = false;
};

struct Observation {
  Vec3 self_pos;
  float self_yaw_deg = 0.f;
  std::vector<EnemySample> enemies;
};

struct PerceptionConfig {
  float fov_half_deg = 52.f;  // ~104 deg total, human-ish game FOV
  float max_range = 90.f;
  float memory_secs = 4.0f;   // how long a lost target stays in mind
  float lock_secs = 0.35f;    // time on target before it can be fought
  float max_dt = 0.1f;        // caps a single tick, so a stalled frame does not
                              // retroactively grant lock-on time
};

struct TrackedTarget {
  int id = 0;
  Vec3 last_seen_pos;
  bool visible = false;
  bool is_teammate = false;
  float health = 100.f;
  // Most recent time this target hurt us. A remembered aggressor outranks a random
  // distant body: it is the one that is actually a threat.
  double last_damaged_me_time = -1e9;
  bool damaged_me_recently(double now, float window_secs = 3.f) const {
    return now - last_damaged_me_time <= window_secs;
  }
  double last_seen_time = -1e9;
  float tracked_for = 0.f;  // consecutive visible seconds
  bool fightable = false;

  float age(double now) const { return static_cast<float>(now - last_seen_time); }
};

class Perception {
 public:
  explicit Perception(PerceptionConfig cfg = {}) : cfg_(cfg) {}

  const PerceptionConfig& config() const { return cfg_; }
  void update(const Observation& obs, double now);

  const std::vector<TrackedTarget>& targets() const { return targets_; }

  // Forget everything. Exists for test scenarios that need a clean slate (loot, heal,
  // rotation) without waiting out the 4 second memory window, and for a respawn where
  // the previous body's tracks are meaningless. Not called from any gameplay path.
  void clear() { targets_.clear(); }
  const TrackedTarget* nearest(bool require_visible) const;
  const TrackedTarget* by_id(int id) const;
  bool can_see(int id, double now, float max_age = 0.f) const;

  // True if the bot currently has line of sight to its own nearest target.
  bool has_visible_target() const { return nearest(true) != nullptr; }

 private:
  Vec3 last_self_pos_{};  // nearest() must rank by distance to SELF, not to world origin
  PerceptionConfig cfg_;
  std::vector<TrackedTarget> targets_;
  double last_time_ = 0.0;
};

}  // namespace ogfn
