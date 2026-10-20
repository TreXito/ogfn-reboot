#pragma once
#include <string>
#include <vector>

#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// What a bot knows about its squad this tick. Deliberately factual: positions,
// health, what each member is shooting at. Judgment belongs in SquadBrain.
struct Teammate {
  int id = 0;
  Vec3 pos;
  float health = 100.f;       // hp + shield, 0..200
  bool downed = false;        // knocked, revivable
  bool is_targeting = false;  // currently shooting at the same target
  int target_id = 0;          // what it says it is fighting, 0 = nothing
  bool revive_in_progress = false;
};

// A coordination decision for this tick. These are intents, not actions: the brain
// still decides whether to fight, and the squad layer only shapes that decision.
struct SquadDirective {
  // Fight this target (id), or 0 for "no opinion". A focus target at half health
  // 10m away outranks a full-health one at 40m even if the second bot saw it first.
  int focus_target_id = 0;

  // Spread away from this position by at least `spacing` metres. Bots that stack in
  // one doorway are obvious, and a grenade that kills two at once is worse.
  bool should_spread = false;
  Vec3 spread_from;
  float spacing = 4.f;

  // Go revive a downed teammate. `revive_x/y` is the body.
  bool should_revive = false;
  Vec3 revive_at;
  int revive_id = 0;

  // Call for help: the bot is fighting and losing, squadmates should close.
  bool requesting_help = false;

  std::string reason;
};

// Squad behaviour. The rules here are the ones that make a group of humans look
// like a group rather than N solo players who happen to be near each other:
//
//   1. Focus fire. Humans shoot the same target, because a target that dies fast is
//      the only way a 2v1 beats two 1v1s. Bots that each pick their own target lose
//      to the same number of players who do not.
//   2. Revive before loot. A downed teammate is a timer, and humans drop everything
//      for it unless the fight is still on top of them.
//   3. Spacing. Never stand inside a teammate: you cannot both shoot, and one
//      grenade takes the squad.
//   4. Help calls. Someone who is losing a fight says so, and the nearest free
//      teammate turns and comes, instead of finishing their own loot.
class SquadBrain {
 public:
  explicit SquadBrain(uint64_t seed) : rng_(seed) {
    // Some players are the ones who go for the revive, others need to be told. Fixed
    // per bot so a squad does not all rush the body at once.
    revive_urge_ = rng_.range(0.45f, 1.f);
  }

  // `teammates` excludes self. `enemy_ids` are this bot's own visible targets.
  SquadDirective update(const std::vector<Teammate>& teammates,
                        const std::vector<int>& visible_enemy_ids,
                        const Vec3& self, float self_health_frac, bool engaged,
                        bool enemies_pushing_me, uint64_t now_ms);

  // Focus target chosen by the squad this tick, for telemetry.
  int focus_target() const { return focus_target_; }

 private:
  Rng rng_;
  float revive_urge_ = 0.7f;
  int focus_target_ = 0;
  // 0 means "never called", which must NOT be treated as "called at t=0": doing so
  // suppressed the first help call for the first 4s of every match.
  uint64_t last_call_ms_ = 0;
  // A revive is a hold, not a lifestyle. Once committed, the bot sticks with the
  // body for a bounded window and then goes back to playing, even if the body is
  // still down (someone else's job, or you cannot reach it).
  uint64_t revive_until_ms_ = 0;
  int revive_target_ = 0;
  bool revive_attempted_ = false;
};

}  // namespace ogfn
