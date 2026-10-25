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
    case ActionKind::Revive: return "Revive";
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

// Expected damage per second against the visible target, as a multiplier on Engage.
//
// Returns 0.35..1.35: cheap to reach and hurtful is above 1, blocked or far and weak
// is well below. It is deliberately NOT a veto: a player will still take a long shot or
// shoot at someone behind a fence, they just weight it lower than a clean close one.
float expected_damage_factor(const WorldSnapshot& w) {
  if (!w.target_is_visible) return 0.35f;
  const float d = clampf(w.target_distance, 1.f, 200.f);

  // Angular size of the body: the dominant term at range. 0.42m radius at 12m is
  // ~2.0deg, at 60m ~0.4deg, which is the whole reason long fights are hard.
  const float body_deg = rad2deg(std::atan2(0.42f, d));
  // A human's own line error does not shrink with distance, so the probability of the
  // crosshair being inside that angle falls roughly with 1/d^2.
  float connect = clampf(body_deg / (body_deg + 0.55f), 0.10f, 0.97f);

  // Cover: the round never arrives.
  if (!w.target_has_line_of_sight) connect *= 0.18f;

  // Weapon falloff, in the same shape the hit model uses.
  const float falloff = d <= 30.f ? 1.f
                      : d >= 90.f ? 0.35f
                      : lerpf(1.f, 0.35f, (d - 30.f) / 60.f);

  // A reloading target is free damage even with a mediocre shot.
  const float freebie = w.target_is_reloading ? 1.12f : 1.f;

  // Map 0..~1 of (connect * falloff) onto the 0.35..1.35 band.
  const float raw = connect * falloff * freebie;
  return clampf(0.35f + raw * 0.95f, 0.35f, 1.35f);
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
      // The score must agree with the valuation, or the brain picks Loot on a COUNT
      // and then make_intent finds nothing worth the walk: "decided to loot" with no
      // looting in it. `c.want` IS that check, and it already applies the per-item
      // radius and the walk cost.
      //
      // There used to be an extra `loot_within_30m <= 0` gate on top, which was
      // redundant AND wrong: it hard-vetoed every item past 30m, so the wider radius
      // that lets an unarmed bot cross ground for a gun could never take effect, and
      // a bot that landed in the open scored Loot at exactly zero and stood there.
      const LootChoice c = loot_.choose(w.loot, w.inventory, w.self_pos,
                                        w.circle_shrink_in_secs, engaged);
      if (!c.want) return 0.f;
      float s = 0.30f * (0.4f + p.greed) * clampf(c.worth * 2.2f, 0.f, 1.f);
      // A player with no weapon is not "considering" loot, they are GOING to get one.
      // A modest multiplier was not enough: the per-persona bias and the per-plan
      // jitter can outvote a bot into Idle, and an unarmed bot that idles is the exact
      // "bots stand there doing nothing" look. Being unarmed is not a preference, it is
      // the one thing that has to be fixed before anything else matters.
      if (w.inventory.weapon_tier <= 0) s *= 3.2f;
      s *= 1.f - clampf(zp * 0.9f, 0.f, 0.95f);
      s *= 1.f - clampf((1.f - hp) * 0.5f, 0.f, 0.5f);
      // Looting while someone is shooting at you is a good way to die, but a flat
      // 0.15x is too absolute: a big shield 8m away at ZERO shield is worth grabbing
      // even with a contact at 40m, and the old constant made Loot score 0.008 in
      // exactly that case, so the bot never replaced a shield it had lost. Scale by
      // how much of a fight it actually is: on top of you is a real reason to leave it,
      // a distant contact is not.
      if (engaged) {
        const float threat = clampf((45.f - w.target_distance) / 37.f, 0.f, 1.f);
        s *= lerpf(1.f, 0.18f, threat);
      }
      return s;
    }

    case ActionKind::RotateToCircle: {
      if (w.distance_to_circle_edge <= -20.f) return 0.02f;
      float s = 0.35f + zp;
      s *= 1.f - p.caution * 0.25f;
      // A fight does NOT outrank dying to the storm. Previously it discounted this to
      // 0.25x, and a bot 20m outside the ring chose to trade shots with an enemy 25m
      // away instead of running: measured over a 50-bot match, the storm killed 1 bot
      // and fights killed 47, when a real BR has the storm take a real share. Deep
      // outside the ring, rotating is the only thing a player does.
      // Being OUTSIDE AT ALL is the threshold, not being far outside. The old ramp
      // needed ~10m before rotate overtook a fight, so a bot 2m out kept trading.
      const float outside = clampf(w.distance_to_circle_edge / 6.f + 0.35f, 0.f, 1.f);
      if (engaged) s *= lerpf(0.25f, 1.45f, outside);
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
      // Can this shot actually CONNECT, and how much will it hurt if it does?
      //
      // Score used to be a pure "how far away is it" curve, so a bot would commit to a
      // target behind a wall at 80m exactly as hard as one at 12m in the open. A player
      // does not: they know a body subtends less angle further out, that their weapon
      // falls off, and that a wall eats the round. Expected damage per second is the
      // honest version of that judgement, and it uses the same hit model the engine
      // side uses to resolve the shots, so the two cannot disagree.
      s *= expected_damage_factor(w);
      s *= lerpf(0.35f, 1.f, clampf(hp * 1.2f, 0.f, 1.f) + p.aggression * 0.3f);
      if (w.target_is_reloading) s *= 1.25f;
      if (w.target_health < 50.f) s *= 1.30f;
      // Standing in the storm to finish a fight gets you killed by the storm. A player
      // keeps shooting only while they can still outrun it.
      // Any time outside the ring, a fight is deprioritised: you cannot win a fight
      // you are losing to the storm.
      const float deep_outside = clampf(w.distance_to_circle_edge / 8.f + 0.3f, 0.f, 1.f);
      s *= lerpf(1.f, 0.45f, deep_outside);
      return s;
    }

    case ActionKind::Build: {
      if (!engaged) return 0.f;
      // Only REACTIVELY. An enemy existing is not a reason to start building: measured in
      // the offline match sim, gating on "engaged" alone produced 257 pieces per bot over
      // one match, which reads as a machine. People build when rounds are landing, or
      // when they are hurt and need a moment.
      if (!w.recently_damaged && w.self_health > 60.f) return 0.f;
      float s = 0.34f * (0.3f + p.build_speed);
      s *= lerpf(1.8f, 0.6f, hp);   // building is for when the trade is lost
      s *= lerpf(1.4f, 0.7f, p.aggression);
      // Cover matters more against someone actually breathing on you.
      s *= clampf(1.f + (20.f - w.target_distance) / 20.f, 0.7f, 1.5f);
      return s;
    }

    case ActionKind::Heal: {
      // Anything usable? Shields first, exactly as the item choice does, or the bot
      // scores a heal it cannot perform.
      const bool can_shield = (w.small_shields > 0 || w.big_shields > 0) && w.self_shield < 100.f;
      const bool can_hp = (w.medkits > 0 || w.bandages > 0) && w.self_health < 100.f;
      if (!can_shield && !can_hp) return 0.f;
      // ...and never while already healing.
      if (heal_left_ > 0.f || w.heal_remaining_secs > 0.f) return 0.f;
      if (engaged && w.target_distance < 45.f) return 0.05f;  // not mid-fight
      float s = 0.40f * (0.5f + p.caution);
      // Health is more urgent than shields: a player at 20hp heals out of fear.
      s *= lerpf(1.5f, 0.4f, hp);
      if (can_hp && w.self_health < 50.f) s *= 1.6f;
      return s;
    }

    case ActionKind::Flee: {
      if (!engaged) return 0.f;
      // Nobody runs from a distant enemy at full health. Without a distance term a
      // 100hp bot fled a target 50m away, which reads as cowardice the game does not
      // have: you disengage from something that can actually hurt you, and the
      // further it is the less it can.
      const float threat = clampf(1.f - (w.target_distance - 8.f) / 32.f, 0.f, 1.f);
      if (threat <= 0.02f) return 0.f;
      float s = 0.20f * (0.3f + p.caution);
      s *= lerpf(2.6f, 0.2f, hp);  // only desperate bots run
      s *= threat;                 // and only from something close
      s *= 1.f - p.aggression * 0.5f;
      // Desperation bonus: at near-death, leaving is better than any trade or build.
      // Without it Build always won for a hurt bot (building is scored highest when
      // low), so nothing ever fled in someone's face at 10hp.
      if (hp < 0.25f) s *= 2.4f;
      return s;
    }

    case ActionKind::ThirdParty: {
      if (w.enemies_currently_fighting_each_other <= 0) return 0.f;
      float s = 0.45f * (0.4f + p.patience) * (0.5f + p.aggression);
      s *= lerpf(1.4f, 0.5f, hp);
      return s;
    }

    case ActionKind::Revive:
      // Never scored: a revive is a squad override, not a utility choice. Keeping it
      // out of the candidate list means the argmax cannot pick it on its own.
      return 0.f;

    case ActionKind::HoldAngle: {
      if (engaged || w.enemies_within_30m <= 0) return 0.f;
      float s = 0.30f * (0.3f + p.patience);
      s *= 1.f - p.aggression * 0.4f;
      return s;
    }
  }
  return 0.f;
}

Decision Brain::make_intent(ActionKind a, const WorldSnapshot& w) {
  Decision d;
  d.action = a;
  // NOTE: loot_choice_ is NOT cleared here. A committed tick re-runs make_intent every
  // frame and does not re-run the valuation (the plan beat did), so clearing would
  // erase the live choice one tick after it was made and every consumer would see
  // "no choice" while the bot is visibly walking to the pot.
  switch (a) {
    case ActionKind::Engage: {
      // A fight is not a reason to walk to the middle of the map. This used to set
      // move_to to the CIRCLE CENTRE, so an engaging bot drifted toward the zone centre
      // while shooting: every bot in every fight pulled toward the same point, which is
      // how a 50-bot fleet ends up in a 27 metre box with everyone dead in one second.
      //
      // A player either holds their ground with cover and strafe, or closes to a
      // sensible fighting distance. So: keep the standoff, and only advance when the
      // target is further than that.
      const float standoff = lerpf(18.f, 9.f, persona_.aggression);
      const float gx = w.target_pos.x - w.self_pos.x;
      const float gy = w.target_pos.y - w.self_pos.y;
      const float glen = std::sqrt(gx * gx + gy * gy);
      if (glen > standoff + 1.f && glen > 1e-3f)
      {
        const float advance = glen - standoff;
        d.move_to = Vec3(w.self_pos.x + (gx / glen) * advance,
                         w.self_pos.y + (gy / glen) * advance, 0.f);
      }
      else
      {
        d.move_to = w.self_pos;   // close enough: hold and fight
      }
      d.fire = true;
      break;
    }
    case ActionKind::Build:
      d.build_wall = true;
      d.fire = true;
      break;
    case ActionKind::Flee:
    case ActionKind::RotateToCircle: {
      // Rotating to the exact centre is a beeline, and a beeline is what every bot
      // does when it has no idea where anyone is. Real players enter at an offset,
      // aim for a spot inside the ring rather than its middle, and leave room for
      // the fight they expect on the way in. The offset here is FIXED per bot
      // (derived from its persona seed, not re-rolled per tick), so a bot's rotation
      // line is consistent across a match the way a person's habits are.
      const float ang = rotation_offset_rad_;
      // 15-55% of the remaining radius off-centre, and always inside the ring.
      const float frac = lerpf(0.15f, 0.55f, persona_.patience);
      const float r = clampf(w.circle_radius * frac, 4.f, 120.f);
      const float tx = w.circle_next_center_x + std::cos(ang) * r;
      const float ty = w.circle_next_center_y + std::sin(ang) * r;
      // Flee is different: run away from where the fight is, not to a nice spot.
      if (a == ActionKind::Flee) {
        d.move_to = Vec3(w.self_pos.x + std::cos(ang + kPi) * 30.f,
                         w.self_pos.y + std::sin(ang + kPi) * 30.f, 0.f);
      } else {
        d.move_to = Vec3(tx, ty, 0.f);
      }
      // Look where you are going. This is the difference between a player who runs
      // toward the zone and a bot that moonwalks toward it with its crosshair on a
      // wall: measured, the crosshair was a median 85 degrees off the travel line.
      const float travel = yaw_between(w.self_pos, d.move_to);
      d.look_yaw_deg = gaze_active_ ? gaze_yaw_deg_ : travel;
      d.face_travel = true;
      break;
    }
    case ActionKind::Heal:
      d.build_wall = true;   // heal behind cover
      d.move_to = w.self_pos; // potions do not walk
      d.look_yaw_deg = w.self_yaw_deg;
      break;
    case ActionKind::Loot: {
      // Loot used to fall through to `default`, so it returned an all-default
      // Decision: move_to (0,0,0). The adapter only moves for Rotate/Engage/Flee,
      // so a bot that decided to loot stood still and did nothing, which read as
      // an idle bot rather than a loot bug.
      const float closing = w.circle_shrink_in_secs;
      loot_choice_ = loot_.choose(w.loot, w.inventory, w.self_pos, closing,
                                  in_combat(w));
      if (loot_choice_.want) {
        d.move_to = loot_choice_.pos;
        // Walking to loot: eyes on the loot.
        d.look_yaw_deg = yaw_between(w.self_pos, d.move_to);
        d.face_travel = true;
      } else {
        // Nothing worth walking to: the honest human response is to keep moving
        // with the zone, not to stand in a field.
        d.move_to = Vec3(w.circle_next_center_x, w.circle_next_center_y, 0.f);
      }
      break;
    }
    case ActionKind::ThirdParty: {
      // Third-partying means going to the fight, not to the circle. It used to
      // fall through to `default` as well, so this action did nothing at all.
      d.move_to = Vec3(w.fight_x, w.fight_y, 0.f);
      // Walking to a fight: watch the fight, not the ground.
      d.look_yaw_deg = yaw_between(w.self_pos, d.move_to);
      d.face_travel = true;
      break;
    }
    case ActionKind::HoldAngle:
      // Holding an angle means holding position: no destination by design, which
      // is exactly what move_to (0,0,0) would otherwise mean by accident.
      d.move_to = w.self_pos;
      // ...but a player holding an angle is LOOKING down it, not at their feet.
      d.look_yaw_deg = gaze_active_ ? gaze_yaw_deg_ : w.self_yaw_deg;
      d.face_travel = false;
      break;
    case ActionKind::Idle:
      d.move_to = w.self_pos;
      // Standing still: the movement layer's shoulder-check, if it has one, is what
      // the view should follow. Nothing to face otherwise.
      d.look_yaw_deg = gaze_active_ ? gaze_yaw_deg_ : w.self_yaw_deg;
      d.face_travel = gaze_active_;
      break;
    case ActionKind::Revive:
      if (squad_directive_.should_revive) d.move_to = squad_directive_.revive_at;
      else d.move_to = w.self_pos;
      d.look_yaw_deg = yaw_between(w.self_pos, d.move_to);
      d.face_travel = true;
      break;
  }
  return d;
}

Decision Brain::think(const WorldSnapshot& w) {
  const bool combat_now = in_combat(w);

  // --- committed heal -----------------------------------------------------------
  // Healing is a timed animation in this game (2s to 5s depending on the item), and a
  // player is locked into it: you get shot out of a potion, you do not cancel it to
  // start firing in the same frame. The old model decided Heal per tick with no
  // duration, so a bot "healed" instantly and re-decided the next tick. Now the
  // decision commits to an item and a duration, and nothing else happens until it
  // finishes or is interrupted by taking damage at close range.
  const float dt_guess = 0.016f;   // think() has no dt; the engine calls at tick rate
  if (heal_left_ > 0.f) {
    heal_left_ -= dt_guess;
    // Interrupted: shot at close range while healing. A player drops the heal to
    // fight; they do not stand in the open finishing a bandage.
    if (heal_left_ > 0.f && combat_now && w.target_distance < 25.f) {
      heal_left_ = 0.f;
      heal_item_ = 0;
    } else if (heal_left_ > 0.f) {
      Decision d;
      d.action = ActionKind::Heal;
      d.reason = "healing";
      d.build_wall = true;          // potions behind cover, as people do
      d.move_to = w.self_pos;       // standing still: healing does not walk
      d.look_yaw_deg = w.self_yaw_deg;
      if (last_face_travel_) {
        d.look_yaw_deg = last_look_yaw_;
        d.face_travel = true;
      }
      return d;
    }
  }
  // Engine-reported heal in progress (the adapter knows the real animation length):
  // do not start anything new while it runs.
  if (w.heal_remaining_secs > 0.f) {
    Decision d;
    d.action = ActionKind::Heal;
    d.reason = "healing";
    d.build_wall = true;
    d.move_to = w.self_pos;
    d.look_yaw_deg = w.self_yaw_deg;
    return d;
  }

  // Squad pass first: it shapes which target to fight and whether a revive outranks
  // everything else. With no teammates in the snapshot this is a no-op, so solo
  // behaviour is bit-for-bit unchanged.
  std::vector<int> visible_ids;
  if (!w.teammates.empty()) {
    for (const auto& t : perception_.targets())
      if (t.visible) visible_ids.push_back(t.id);
  }
  squad_directive_ = squad_.update(w.teammates, visible_ids, w.self_pos,
                                   health_fraction(w), combat_now, w.enemies_pushing_me,
                                   w.now_ms);

  // Between plan beats the bot is committed. This is the main difference
  // between human cadence and 60Hz utility spam.
  //
  // GUARD: commitment is bounded in TIME, not only by the clock the caller supplies.
  // If the engine hands the brain a repeated or non-advancing timestamp (a paused
  // sim, a hitched frame, a harness that ticks without advancing `time`), then
  // `w.time` never reaches `next_plan_time_` and the bot stays committed FOREVER:
  // it walks one stale intent for the rest of the match. Live evidence of the shape:
  // a fleet that fired 20,315 shots and then never fired again. A committed plan
  // therefore also expires after a fixed number of ticks, whatever the clock says.
  if (has_decided_ && w.time < next_plan_time_ && committed_ticks_ < kMaxCommittedTicks) {
    ++committed_ticks_;
    Decision d = make_intent(last_action_, w);
    d.reason = "committed";
    d.score = last_score_;
    // Keep the gaze that was PLANNED, not a freshly derived one. make_intent recomputes
    // look_yaw from this tick's move_to, and for a rotation that target is re-derived
    // every tick while the planned gaze was fixed at the plan beat: the two disagreed
    // by a constant 58 degrees. Where you are looking is a decision, and a committed
    // bot does not change its mind about it mid-beat.
    if (last_face_travel_) {
      d.look_yaw_deg = last_look_yaw_;
      d.face_travel = true;
    }
    return d;
  }
  next_plan_time_ = w.time + rng_.range(plan_fast_secs_, plan_slow_secs_);
  committed_ticks_ = 0;

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

  // Indecision. When two options are close, a person sometimes takes the second
  // one: they were already leaning that way, or they misjudge which is better.
  // A pure argmax never does this, and "always picks the maximum" is measurably
  // detectable across a match (it is what made 64 bots agree on 72% of ticks).
  // Bounded on purpose: only near-ties, never a clearly better option.
  if (best.scores.size() >= 2) {
    const float top = best.scores[0].second;
    const float second = best.scores[1].second;
    if (second > 0.f && (top - second) < top * 0.12f && rng_.unit() < 0.25f) {
      // Take the runner-up, and record that it was a coin flip rather than a win.
      for (ActionKind k : candidates) {
        if (to_string(k) == best.scores[1].first) {
          best.action = k;
          best.score = second;
          break;
        }
      }
    }
  }

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
    if (last_face_travel_) {
      d.look_yaw_deg = last_look_yaw_;
      d.face_travel = true;
    }
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
  // A revive outranks any combat or loot plan: it is a timer, and the squad layer
  // already decided the fight is not on top of the body. This is the one case where
  // a squad changes the action rather than shaping it.
  if (squad_directive_.should_revive) {
    Decision revive = make_intent(ActionKind::Revive, w);
    revive.reason = "revive squadmate";
    revive.score = best.score;
    revive.scores = std::move(best.scores);
    // Not committed to the revive action kind: the bot is walking to a body, and the
    // next plan beat re-evaluates whether the body is still down.
    last_action_ = ActionKind::Revive;
    last_score_ = best.score;
    has_decided_ = true;
    return revive;
  }

  Decision planned = make_intent(best.action, w);
  // Committing to a heal means choosing WHICH consumable and for how long. A player
  // tops off shields with the cheap one, saves the big pot for a real hole, and
  // uses a medkit only when health is actually missing.
  if (best.action == ActionKind::Heal && heal_left_ <= 0.f) {
    const bool need_hp = w.self_health < 75.f;
    const bool need_shield = w.self_shield < 100.f;
    if (need_shield && w.small_shields > 0) {
      heal_item_ = 1;
      heal_left_ = 2.0f;
    } else if (need_shield && w.big_shields > 0) {
      heal_item_ = 2;
      heal_left_ = 4.0f;
    } else if (need_hp && w.medkits > 0) {
      heal_item_ = 3;
      heal_left_ = 5.0f;
    } else if (need_hp && w.bandages > 0) {
      heal_item_ = 4;
      heal_left_ = 2.5f;
    }
    // Variance, because nobody's potion takes exactly 2.000 seconds.
    if (heal_left_ > 0.f) heal_left_ *= rng_.range(0.92f, 1.12f);
  }
  last_look_yaw_ = planned.look_yaw_deg;
  last_face_travel_ = planned.face_travel;
  planned.score = best.score;
  planned.scores = std::move(best.scores);
  planned.reason = "plan";
  return planned;
}

}  // namespace ogfn
