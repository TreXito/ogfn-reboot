#include "ogfn/brain.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ogfn {

const char* to_string(ActionKind k) {
  switch (k) {
    case ActionKind::Idle: return "Idle";
    case ActionKind::Loot: return "Loot";
    case ActionKind::RotateToCircle: return "RotateToCircle";
    case ActionKind::Engage: return "Engage";
    case ActionKind::Build: return "Build";
    case ActionKind::Heal: return "Heal";
    case ActionKind::Flee: return "Flee";
    case ActionKind::ThirdParty: return "ThirdParty";
    case ActionKind::HoldAngle: return "HoldAngle";
  }
  return "?";
}

namespace {
// Survival pressure: small near the zone edge, large outside the zone.
float zone_pressure(const WorldSnapshot& w) {
  const float d = w.distance_to_circle_edge;  // negative inside
  if (d <= 0.f) {
    const float margin = clampf(-d / 40.f, 0.f, 1.f);
    return 0.25f * (1.f - margin);
  }
  return 0.7f + clampf(d / 60.f, 0.f, 1.f) * 1.6f;
}

float health_fraction(const WorldSnapshot& w) {
  return clampf((w.self_health + w.self_shield) / 200.f, 0.f, 1.f);
}

bool in_combat(const WorldSnapshot& w) {
  return w.target_is_visible && w.target_distance < 70.f;
}
}  // namespace

float Brain::score(ActionKind k, const WorldSnapshot& w) const {
  const Persona& p = persona_;
  const float hp = health_fraction(w);
  const float zp = zone_pressure(w);
  const bool engaged = in_combat(w);

  switch (k) {
    case ActionKind::Idle:
      return 0.05f;

    case ActionKind::Loot: {
      if (w.loot_within_30m <= 0) return 0.f;
      float s = 0.30f * (0.4f + p.greed) *
                clampf(static_cast<float>(w.loot_within_30m) / 3.f, 0.f, 1.f);
      s *= 1.f - clampf(zp * 0.9f, 0.f, 0.95f);
      s *= 1.f - clampf((1.f - hp) * 0.5f, 0.f, 0.5f);
      if (engaged) s *= 0.15f;
      return s;
    }

    case ActionKind::RotateToCircle: {
      if (w.distance_to_circle_edge <= -20.f) return 0.02f;
      float s = 0.35f + zp;
      s *= 1.f - p.caution * 0.25f;
      if (engaged) s *= 0.25f;
      return s;
    }

    case ActionKind::Engage: {
      if (!engaged || !w.has_weapon) return 0.f;
      float s = 0.55f * (0.5f + p.aggression);
      s *= 0.6f + 0.4f * p.aim_skill;
      // Urgency scales with proximity, but the falloff is sharp: a person 12m
      // away matters far more than one at 50m. Without this every bot engages
      // every visible enemy at any range, which reads as a terminator, not a
      // player. Distant contacts are only fought when nothing better is going on.
      const float proximity = clampf(1.f + (25.f - w.target_distance) / 25.f, 0.5f, 1.7f);
      s *= proximity;
      s *= lerpf(0.35f, 1.f, clampf(hp * 1.2f, 0.f, 1.f) + p.aggression * 0.3f);
      if (w.target_is_reloading) s *= 1.25f;
      if (w.target_health < 50.f) s *= 1.30f;
      return s;
    }

    case ActionKind::Build: {
      if (!engaged) return 0.f;
      float s = 0.34f * (0.3f + p.build_speed);
      s *= lerpf(1.8f, 0.6f, hp);   // building is for when the trade is lost
      s *= lerpf(1.4f, 0.7f, p.aggression);
      // Cover matters more against someone actually breathing on you.
      s *= clampf(1.f + (20.f - w.target_distance) / 20.f, 0.7f, 1.5f);
      return s;
    }

    case ActionKind::Heal: {
      if (w.self_shield >= 100.f || w.shield_potions <= 0) return 0.f;
      if (engaged && w.target_distance < 45.f) return 0.05f;  // not mid-fight
      float s = 0.40f * (0.5f + p.caution);
      s *= lerpf(1.5f, 0.4f, hp);
      return s;
    }

    case ActionKind::Flee: {
      if (!engaged) return 0.f;
      float s = 0.20f * (0.3f + p.caution);
      s *= lerpf(2.6f, 0.2f, hp);  // only desperate bots run
      s *= 1.f - p.aggression * 0.5f;
      return s;
    }

    case ActionKind::ThirdParty: {
      if (w.enemies_currently_fighting_each_other <= 0) return 0.f;
      float s = 0.45f * (0.4f + p.patience) * (0.5f + p.aggression);
      s *= lerpf(1.4f, 0.5f, hp);
      return s;
    }

    case ActionKind::HoldAngle: {
      if (engaged || w.enemies_within_30m <= 0) return 0.f;
      float s = 0.30f * (0.3f + p.patience);
      s *= 1.f - p.aggression * 0.4f;
      return s;
    }
  }
  return 0.f;
}

Decision Brain::make_intent(ActionKind a, const WorldSnapshot& w) const {
  Decision d;
  d.action = a;
  switch (a) {
    case ActionKind::Engage:
      d.move_to = Vec3(w.circle_center_x, w.circle_center_y, 0.f);
      d.fire = true;
      break;
    case ActionKind::Build:
      d.build_wall = true;
      d.fire = true;
      break;
    case ActionKind::Flee:
    case ActionKind::RotateToCircle:
      d.move_to = Vec3(w.circle_next_center_x, w.circle_next_center_y, 0.f);
      break;
    case ActionKind::Heal:
      d.build_wall = true;  // heal behind cover
      break;
    default:
      break;
  }
  return d;
}

Decision Brain::think(const WorldSnapshot& w) {
  const bool combat_now = in_combat(w);

  // Between plan beats the bot is committed. This is the main difference
  // between human cadence and 60Hz utility spam.
  if (has_decided_ && w.time < next_plan_time_) {
    Decision d = make_intent(last_action_, w);
    d.reason = "committed";
    d.score = last_score_;
    return d;
  }
  next_plan_time_ = w.time + rng_.range(plan_fast_secs_, plan_slow_secs_);

  const ActionKind candidates[] = {
      ActionKind::Idle,      ActionKind::Loot,       ActionKind::RotateToCircle,
      ActionKind::Engage,    ActionKind::Build,      ActionKind::Heal,
      ActionKind::Flee,      ActionKind::ThirdParty, ActionKind::HoldAngle,
  };

  Decision best;
  best.score = -1.f;
  int idx = 0;
  for (ActionKind k : candidates) {
    const int bi = idx++ % kActionCount;
    const float base = score(k, w);
    if (base <= 0.f) continue;
    // Idiosyncrasy (fixed per bot) times per-plan jitter. The fixed part makes a
    // given bot consistently favour certain actions, so two equally skilled bots
    // still play differently, and predictably differently.
    const float noisy = base * persona_.bias[bi] * (1.f + rng_.normal(0.f, 0.12f));
    best.scores.emplace_back(to_string(k), noisy);
    if (noisy > best.score) {
      best.score = noisy;
      best.action = k;
    }
  }
  std::sort(best.scores.begin(), best.scores.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Surprise handling. Only a change in combat involvement costs reaction time;
  // ordinary task changes are taken up at the next plan beat. One episode, fixed
  // deadline, never extended (extending it is what freezes a bot solid).
  const bool surprise = has_decided_ && combat_now != engaged_ && best.action != last_action_;
  if (surprise && !in_reaction_) {
    in_reaction_ = true;
    reaction_deadline_ = w.time + sample_reaction_secs();
  }
  bool held_by_reaction = false;
  if (in_reaction_) {
    if (w.time < reaction_deadline_) {
      best.action = last_action_;
      held_by_reaction = true;
    } else {
      in_reaction_ = false;
    }
  }

  engaged_ = combat_now;
  last_action_ = best.action;
  last_score_ = best.score;
  has_decided_ = true;

  if (held_by_reaction) {
    Decision d = make_intent(last_action_, w);
    d.reason = "reaction delay";
    d.score = last_score_;
    d.scores = best.scores;
    return d;
  }

  // The plan path must build the intent too. Returning `best` raw leaves
  // move_to at the default (0,0,0), so every plan beat sends the bot to the
  // world origin: live bots walked at the corner of the map on ~12% of ticks
  // while committed ticks were fine, which is why it read as an intermittent
  // movement bug instead of a missing call.
  Decision planned = make_intent(best.action, w);
  planned.score = best.score;
  planned.scores = std::move(best.scores);
  planned.reason = "plan";
  return planned;
}

}  // namespace ogfn
