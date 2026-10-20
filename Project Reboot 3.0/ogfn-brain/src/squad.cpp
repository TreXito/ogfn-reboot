#include "ogfn/squad.h"

#include <algorithm>
#include <cmath>

namespace ogfn {

namespace {
// Spacing: closer than this and the squad is one target, not a squad.
constexpr float kMinSpacing = 4.f;
// How often a bot may shout for help. People do not spam it.
constexpr uint64_t kHelpCooldownMs = 4000;
// How long a committed revive lasts before the bot gives up and plays on. Long
// enough for a real revive hold plus the walk, short enough that a bot does not
// spend the rest of the match standing on a body it cannot reach.
constexpr uint64_t kReviveHoldMs = 14000;
}  // namespace

SquadDirective SquadBrain::update(const std::vector<Teammate>& teammates,
                                  const std::vector<int>& visible_enemy_ids,
                                  const Vec3& self, float self_health_frac, bool engaged,
                                  bool enemies_pushing_me, uint64_t now_ms) {
  SquadDirective d;

  // --- 1. focus fire -----------------------------------------------------------
  // Prefer a target a teammate is already shooting: two guns on one body kills it
  // twice as fast, and kills are what win fights. Ties break to the lowest id so
  // the whole squad converges instead of splitting.
  int called = 0;
  for (const auto& t : teammates) {
    if (t.target_id == 0) continue;
    const bool visible_to_me =
        std::find(visible_enemy_ids.begin(), visible_enemy_ids.end(), t.target_id) !=
        visible_enemy_ids.end();
    // You cannot shoot what you cannot see, so a called target only becomes your
    // focus if it is in your own list. Otherwise you would walk at a wall.
    if (!visible_to_me) continue;
    if (called == 0 || t.target_id < called) called = t.target_id;
  }
  if (called != 0) {
    d.focus_target_id = called;
    d.reason = "focus fire with squad";
  } else if (!visible_enemy_ids.empty()) {
    // Nobody is calling anything: take the lowest id, and everyone doing this
    // converges on the same body without any explicit messaging.
    int lowest = visible_enemy_ids.front();
    for (int id : visible_enemy_ids) lowest = std::min(lowest, id);
    d.focus_target_id = lowest;
    d.reason = "no call, converging on lowest id";
  }
  focus_target_ = d.focus_target_id;

  // --- 2. revive before loot ---------------------------------------------------
  // Only when the bot is not itself in a fight, and the body is not sitting in one.
  // The latch is per body: once that body is no longer down (revived, or it died and
  // left), the bot is free to attempt a revive again on the next one it sees.
  if (revive_attempted_ && revive_target_ != 0) {
    bool still_down = false;
    for (const auto& t : teammates)
      if (t.id == revive_target_ && t.downed) still_down = true;
    if (!still_down) {
      revive_attempted_ = false;
      revive_until_ms_ = 0;
      revive_target_ = 0;
    }
  }

  const Teammate* best_body = nullptr;
  float best_score = 0.f;
  for (const auto& t : teammates) {
    if (!t.downed || t.revive_in_progress) continue;   // someone else has it
    const float dist = length(t.pos - self);
    // Closer bodies first; beyond 40m it is not your job, someone nearer has it.
    if (dist > 40.f) continue;
    float score = 1.f / (1.f + dist / 10.f);
    // A bot that is itself nearly dead is a liability on a revive.
    score *= lerpf(0.25f, 1.f, clampf(self_health_frac, 0.f, 1.f));
    score *= revive_urge_;
    if (score > best_score) {
      best_score = score;
      best_body = &t;
    }
  }

  // A revive is one committed episode, not a mode. Once the hold window closes the
  // bot plays on EVEN IF the body is still down: a bot that stands on an unreachable
  // or already-handled body for the rest of the match is a stuck bot, and that is
  // what an unbounded revive rule produced (100% of ticks in a 10s window).
  const bool holding_revive = revive_until_ms_ != 0 && now_ms < revive_until_ms_;

  if (best_body && !engaged && holding_revive) {
    d.should_revive = true;
    d.revive_at = best_body->pos;
    d.revive_id = best_body->id;
    d.reason = "squadmate down";
    // Reviving is the plan: do not also call a target in the same tick, or the bot
    // walks at one thing and shoots at another.
    d.focus_target_id = 0;
    focus_target_ = 0;
  } else if (best_body && !engaged && !holding_revive && !revive_attempted_) {
    // First sighting of a body: commit to it for the hold window.
    revive_until_ms_ = now_ms + kReviveHoldMs;
    revive_target_ = best_body->id;
    revive_attempted_ = true;   // one attempt per body, not a loop
    d.should_revive = true;
    d.revive_at = best_body->pos;
    d.revive_id = best_body->id;
    d.reason = "squadmate down";
    d.focus_target_id = 0;
    focus_target_ = 0;
  } else if (best_body && engaged && enemies_pushing_me) {
    // Being pushed while a body is down: fight first. Humans clear the threat and
    // then revive, and the revive window is usually long enough.
    d.reason = "body down but the fight is live";
  }

  // --- 3. spacing --------------------------------------------------------------
  // Never stand on a teammate. Push away from the nearest one, scaled by how close
  // it is, so two bots in the same doorway separate on their own.
  const Teammate* nearest = nullptr;
  float nearest_dist = 1e9f;
  for (const auto& t : teammates) {
    if (t.pos.x == self.x && t.pos.y == self.y) continue;
    const float dist = length(t.pos - self);
    if (dist < nearest_dist) {
      nearest_dist = dist;
      nearest = &t;
    }
  }
  if (nearest && nearest_dist < kMinSpacing) {
    d.should_spread = true;
    d.spread_from = nearest->pos;
    d.spacing = kMinSpacing;
    if (d.reason.empty()) d.reason = "too close to a squadmate";
  }

  // --- 4. help call ------------------------------------------------------------
  // A bot that is losing says so, no more than once every few seconds. The call is
  // advisory: it does not override another bot's revive, and it is only made when
  // the bot is actually engaged and hurt.
  // NB: the first call must go out immediately. Comparing against a 0-initialised
  // timestamp suppressed every call for the first 4 seconds of a match, which is
  // exactly when the first fight happens.
  if (engaged && enemies_pushing_me && self_health_frac < 0.55f &&
      (last_call_ms_ == 0 || now_ms - last_call_ms_ > kHelpCooldownMs)) {
    last_call_ms_ = now_ms;
    d.requesting_help = true;
    if (d.reason.empty()) d.reason = "losing this fight";
  }

  return d;
}

}  // namespace ogfn
