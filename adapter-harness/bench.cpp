// Batch runner: plays N bot lives through a scripted scenario, then prints
// behavioural telemetry. This is the loop an agent uses to keep tuning the bot
// brain without a game engine, and the same telemetry a human uses to see
// whether the bots stopped feeling robotic.
//
// Build: make bench
// Usage: ./build/bench [lives] [minutes] [seed]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "ogfn/ogfn_bots.h"

using namespace ogfn;

namespace {

struct Stats {
  std::map<std::string, int> action_counts;
  std::vector<float> shot_intervals;
  std::vector<float> reaction_times;
  float time_hurt = 0.f;
  float time_safe = 0.f;
  int shots = 0;
  int frames = 0;
  int decisions = 0;
  int plan_ticks = 0;
  int committed_ticks = 0;
  int reaction_held = 0;
  int build_inputs = 0;
  float aim_error_sum = 0.f;
  int aim_error_n = 0;
  int trigger_ticks = 0;
  int trigger_windows = 0;
  int bursts = 0;
  // Movement telemetry. These are the numbers that say whether a bot in a fight
  // looks alive, so they belong in the same report as aim and trigger data.
  int strafe_ticks = 0;
  int crouch_ticks = 0;
  int crouch_changes = 0;
  int jumps = 0;
  int reloads = 0;
  int dry_ticks = 0;
  int build_pieces = 0;
  int build_edits = 0;
  int boxes = 0;
  BuildGoal last_goal = BuildGoal::None;
  // Loot telemetry: a bot that never loots is not playing the game, one that loots
  // constantly never fights, and one whose Loot decisions have nowhere to go is the
  // (0,0,0) bug class.
  int loot_decisions = 0;
  int loot_no_target = 0;
  int loot_walked_to_origin = 0;
  float loot_worth_sum = 0.f;
  int loot_kinds[13] = {0};
  // Squad telemetry. Without teammates in the scripted world the solo path is what
  // gets exercised, so these count the squad-shaped intents that DID appear.
  int revives = 0;          // revive EPISODES (a 14s hold is one, not 840 ticks)
  bool in_revive = false;
  int focus_calls = 0;
  int spread_orders = 0;
  int help_calls = 0;
  // Gaze: how far the view sits from the travel line when not fighting. A bot that
  // walks sideways with its crosshair on nothing is a tell (previously the median
  // offset was 85 degrees).
  // Store every sample, because the MEDIAN is the right statistic here: a shoulder
  // check legitimately puts the view 70 degrees off travel for a second, and a mean
  // punishes that. What matters is that the bot is usually facing where it walks.
  std::vector<float> gaze_offs;
  bool last_crouch = false;
  std::vector<float> burst_lengths;
};

// A synthetic match: the world evolves on a schedule so the bot experiences
// quiet periods, sudden contacts, a storm and a scramble, in a repeatable way.
WorldSnapshot scripted_world(double t, uint64_t life) {
  WorldSnapshot w;
  w.time = t;
  const double phase = std::fmod(t, 60.0);
  w.has_weapon = true;

  // Storm: closes slowly, forcing rotations twice a minute.
  w.circle_center_x = static_cast<float>(60.0 * std::sin(t / 37.0));
  w.circle_center_y = static_cast<float>(60.0 * std::cos(t / 41.0));
  w.circle_next_center_x = w.circle_center_x * 0.7f;
  w.circle_next_center_y = w.circle_center_y * 0.7f;
  w.circle_radius = static_cast<float>(220.0 - std::min(t / 3.0, 160.0));

  const float self_r = static_cast<float>(std::hypot(w.circle_center_x, w.circle_center_y)) * 0.f;
  (void)self_r;
  w.distance_to_circle_edge = static_cast<float>(40.0 * std::sin(t / 23.0)) - 15.f;

  // Loot and enemies come in waves.
  w.loot_within_30m = static_cast<int>(3.f + 3.f * std::sin(t / 11.0));
  if (w.loot_within_30m < 0) w.loot_within_30m = 0;
  // Real pickups, because a count cannot tell a big pot from an ammo box and the
  // decision is made on the objects. Positions ride the same slow wave as the
  // count so the bench stays repeatable.
  w.loot.clear();
  for (int li = 0; li < w.loot_within_30m; ++li) {
    LootSample ls;
    ls.id = 900 + li + (int)(life % 7) * 10;
    const float ang = static_cast<float>(li) * 2.1f + static_cast<float>(t) * 0.05f;
    const float rad = 6.f + 5.f * static_cast<float>(li);
    ls.pos = Vec3(std::cos(ang) * rad, std::sin(ang) * rad, 0.f);
    // A rotating mix of classes so every bot meets guns, heals and clutter.
    switch ((li + (int)(life % 5)) % 6) {
      case 0: ls.cls = ItemClass::ShieldBig; break;
      case 1: ls.cls = ItemClass::Ammo; ls.count = 30; break;
      case 2: ls.cls = ItemClass::Bandage; ls.count = 5; break;
      case 3: ls.cls = ItemClass::Shotgun; break;
      case 4: ls.cls = ItemClass::ShieldPotion; break;
      default: ls.cls = ItemClass::Materials; ls.count = 40; break;
    }
    w.loot.push_back(ls);
  }
  // Inventory evolves so looting is a real decision, not a constant. Shield cycles
  // between fights; the gun situation is fixed per life.
  w.inventory.shield = static_cast<int>(clampf(60.f * static_cast<float>(std::sin(t / 9.0)) + 40.f, 0.f, 100.f));
  w.inventory.big_shields = (static_cast<int>(t) / 30) % 3;
  w.inventory.small_shields = (static_cast<int>(t) / 20) % 4;
  w.inventory.weapon_tier = (life % 3 == 0) ? 0 : 2;
  w.inventory.has_shotgun = (life % 2) == 0;
  w.inventory.ammo = 40 + static_cast<int>(t) % 60;
  w.fight_x = static_cast<float>(30.0 * std::sin(t / 13.0));
  w.fight_y = static_cast<float>(30.0 * std::cos(t / 17.0));
  w.enemies_within_30m = static_cast<int>(2.f + 2.f * std::sin(t / 7.0));
  if (w.enemies_within_30m < 0) w.enemies_within_30m = 0;
  w.enemies_currently_fighting_each_other = (std::fmod(t, 17.0) < 4.0) ? 2 : 0;

  // Contacts: periodic fights, longer every third one.
  const double contact_period = 26.0;
  const double since = std::fmod(t + static_cast<double>(life % 5) * 3.0, contact_period);
  const bool in_contact = since < (life % 3 == 0 ? 11.0 : 6.0);
  w.target_is_visible = in_contact;
  w.target_distance = in_contact ? static_cast<float>(8.0 + 30.0 * (since / contact_period)) : 999.f;
  w.target_health = 100.f - static_cast<float>(std::fmod(since * 9.0, 80.0));
  w.target_is_reloading = std::fmod(since, 3.0) < 0.8;

  // Health: drains during contact, recovers in quiet periods.
  w.self_health = in_contact ? static_cast<float>(35.0 + 40.0 * std::sin(t / 5.0)) : 95.f;
  w.self_health = clampf(w.self_health, 5.f, 100.f);
  w.self_shield = in_contact ? static_cast<float>(20.f + 60.f * std::sin(t / 9.0)) : 100.f;
  w.self_shield = clampf(w.self_shield, 0.f, 100.f);
  w.shield_potions = in_contact ? 0 : 3;

  (void)phase;
  return w;
}

Stats run_life(uint64_t seed, double minutes, bool in_squad = false) {
  Stats s;
  Rng seeder(seed * 2654435761ull + 12345ull);
  const Persona p = Persona::sample(seeder, 0.f);
  Brain brain(p, seed);
  Motor motor(p, seed ^ 0x9E3779B9ull);
  Locomotion loco(p, seed ^ 0x51ED270Bull);
  motor.set_weapon(WeaponProfile(30, 2.6f));
  Builder builder(p, seed ^ 0xB0117ull);

  const float dt = 1.f / 60.f;
  const int ticks = static_cast<int>(minutes * 60.0 / dt);
  float yaw = 0.f;
  double last_shot = -1e9;
  bool was_firing = false;
  float burst_frames = 0.f;
  Vec3 self_pos{0.f, 0.f, 0.f};

  for (int i = 0; i < ticks; ++i) {
    const double t = static_cast<double>(i) * static_cast<double>(dt);
    WorldSnapshot w = scripted_world(t, seed);
    // Squad context: half the lives run as a pair, so the squad layer is actually
    // exercised instead of only proven inert. The teammate is a synthetic body that
    // goes down periodically, which is the only way a revive can be measured.
    if (in_squad) {
      w.teammates.clear();
      Teammate mate;
      mate.id = 5000 + static_cast<int>(seed % 97);
      // A teammate orbiting the bot at 3 to 9 m: close enough to trigger spacing.
      const float ang = static_cast<float>(t) * 0.3f;
      const float rad = 3.f + 6.f * (0.5f + 0.5f * static_cast<float>(std::sin(t / 5.0)));
      mate.pos = Vec3(w.self_pos.x + std::cos(ang) * rad,
                      w.self_pos.y + std::sin(ang) * rad, 0.f);
      mate.health = 200.f;
      // Down for a 4s window every 40s, outside of contacts so a revive is correct.
      mate.downed = std::fmod(t, 40.0) > 30.0 && !w.target_is_visible;
      w.teammates.push_back(mate);
      w.enemies_pushing_me = w.target_is_visible && std::fmod(t, 7.0) < 2.0;
      w.now_ms = static_cast<uint64_t>(t * 1000.0);
    }
    // Perception needs an actual enemy sample for the squad layer to call a focus
    // target: it only focuses on things this bot can SEE. Without this the focus
    // path is never exercised and "squad focus fire" would be an unmeasured claim.
    {
      Observation obs;
      obs.self_pos = w.self_pos;
      obs.self_yaw_deg = 0.f;
      if (w.target_is_visible) {
        EnemySample e;
        e.id = 700 + static_cast<int>(seed % 13);
        e.pos = Vec3(w.self_pos.x + w.target_distance, w.self_pos.y, 0.f);
        e.line_of_sight = true;
        obs.enemies.push_back(e);
      }
      brain.observe(obs, t);
    }
    // Locomotion runs BEFORE the brain: the brain consumes the gaze direction and the
    // travel target, so the view and the legs have to be resolved first. This is also
    // what a real client does (input -> movement state -> decision -> aim).
    const bool engaged_now = w.target_is_visible && w.target_distance < 70.f;
    const float travel_yaw = yaw_between(w.self_pos, w.circle_next_center_x == 0.f &&
                                                            w.circle_next_center_y == 0.f
                                             ? Vec3(1.f, 0.f, 0.f)
                                             : Vec3(w.circle_next_center_x,
                                                    w.circle_next_center_y, 0.f));
    MoveState ms;
    ms.airborne = false;
    ms.has_cover = w.target_is_visible && w.target_distance < 30.f && (i % 240 < 90);
    ms.on_slope = (i % 900) > 780;
    const float health_frac_pre = clampf((w.self_health + w.self_shield) / 200.f, 0.f, 1.f);
    const MoveOutput mo = loco.update(dt, yaw,
                                      w.target_is_visible ? yaw_between(self_pos, Vec3(w.target_distance, 0.f, 0.f)) : yaw,
                                      w.target_distance, engaged_now, false,
                                      w.target_is_reloading, health_frac_pre, ms, travel_yaw, /*travel_supplied*/ true);
    brain.set_gaze(mo.gaze_weight > 0.f, mo.gaze_yaw_deg);
    w.travelling = true;
    w.travel_to = Vec3(w.self_pos.x + std::cos(deg2rad(travel_yaw)) * 10.f,
                       w.self_pos.y + std::sin(deg2rad(travel_yaw)) * 10.f, 0.f);

    const Decision d = brain.think(w);
    ++s.frames;
    s.action_counts[to_string(d.action)]++;
    if (d.reason == "plan") ++s.plan_ticks;
    if (d.reason == "committed") ++s.committed_ticks;
    if (d.reason == "reaction delay") ++s.reaction_held;

    if (w.target_is_visible) s.time_hurt += dt; else s.time_safe += dt;

    // Motor: aim and fire at the target when engaged, otherwise scan.
    MotorOutput out;
    if (d.fire && w.target_is_visible && w.target_distance < 80.f) {
      const Vec3 target{w.target_distance, 0.f, 0.f};
      out = motor.aim_at(self_pos, yaw, target, true, dt);
      ++s.trigger_windows;
      if (out.firing) {
        ++s.shots;
        ++s.trigger_ticks;
        if (!was_firing) ++s.bursts;
        if (was_firing && last_shot > -1e8)
          burst_frames += 1.f;
      } else if (was_firing) {
        // A burst just ended: record its length in seconds.
        s.burst_lengths.push_back(burst_frames * dt);
        burst_frames = 0.f;
      }
      was_firing = out.firing;
      last_shot = t;
    } else {
      if (was_firing) {
        s.burst_lengths.push_back(burst_frames * dt);
        burst_frames = 0.f;
        was_firing = false;
      }
      out = motor.idle_look(yaw, dt);
    }
    yaw = out.yaw_deg;
    s.aim_error_sum += motor.aim_error_deg();
    ++s.aim_error_n;
    s.build_inputs += motor.build_inputs_this_tick(dt);

    // Locomotion already ran above (before the decision).
    if (mo.strafing) ++s.strafe_ticks;
    if (mo.crouching) ++s.crouch_ticks;
    if (mo.crouching != s.last_crouch) ++s.crouch_changes;
    s.last_crouch = mo.crouching;
    if (mo.jumping) ++s.jumps;
    s.reloads = motor.reload_count();
    if (motor.reloading()) ++s.dry_ticks;

    // Building: the brain's Build action decides the intent, the builder decides
    // the pieces. Edits matter as much as placements: a bot that walls up and
    // then walks around its own wall is not building, it is hiding.
    const bool taking_fire = w.target_is_visible && w.self_health < 70.f;
    const BuildInput bi = builder.update(dt, d.action == ActionKind::Build, taking_fire,
                                         health_frac_pre, 0.f, true);
    // Gaze alignment while travelling out of combat. Measured against the travel
    // direction the movement layer was given (travel_yaw), which is what the bot
    // decided to walk toward; d.move_to is the rotation TARGET (offset inside the
    // ring), so comparing to that would measure the offset, not the gaze.
    if (d.face_travel && !(d.action == ActionKind::Engage) && !(d.action == ActionKind::Build)) {
      const float off = std::fabs(wrap180(d.look_yaw_deg - travel_yaw));
      if (off < 180.f) {
        s.gaze_offs.push_back(off);
      }
    }

    // Squad: focus calls and revives are the two that change what a bot does.
    if (brain.squad_directive().focus_target_id != 0) ++s.focus_calls;
    // Count episodes, not ticks: "revives per minute" is a rate people can reason
    // about, and a per-tick count made a healthy 14s hold look like 840 revives.
    const bool reviving = d.action == ActionKind::Revive;
    if (reviving && !s.in_revive) ++s.revives;
    s.in_revive = reviving;
    if (brain.squad_directive().should_spread) ++s.spread_orders;
    if (brain.squad_directive().requesting_help) ++s.help_calls;

    // Loot: did the decision carry a destination, and what did it pick?
    if (d.action == ActionKind::Loot && d.reason == "plan") {
      ++s.loot_decisions;
      const LootChoice& lc = brain.loot_choice();
      if (lc.want) {
        s.loot_worth_sum += lc.worth;
        ++s.loot_kinds[static_cast<int>(lc.cls) % 13];
      } else {
        ++s.loot_no_target;
      }
      // The origin is a legitimate destination only if the circle happens to be
      // there; in this scenario the circle moves, so (0,0) means the intent was
      // never filled in.
      if (!lc.want && d.move_to.x == 0.f && d.move_to.y == 0.f) ++s.loot_walked_to_origin;
    }
    if (bi.piece != PieceKind::None) ++s.build_pieces;
    if (bi.edit) ++s.build_edits;
    if (builder.goal() == BuildGoal::BoxUp && s.last_goal != BuildGoal::BoxUp) ++s.boxes;
    s.last_goal = builder.goal();

    // Reaction telemetry: how long the bot took to adopt an intent after a
    // surprise contact, using the brain's own gate as the witness.
    if (brain.is_reacting(t)) ++s.decisions;
  }

  // Derive per-life reaction estimate from the exposed deadline.
  return s;
}

float pct(const std::vector<float>& v, float q) {
  if (v.empty()) return 0.f;
  std::vector<float> c = v;
  std::sort(c.begin(), c.end());
  const size_t i = static_cast<size_t>(q * static_cast<float>(c.size() - 1));
  return c[i];
}

}  // namespace

int main(int argc, char** argv) {
  const int lives = argc > 1 ? std::atoi(argv[1]) : 64;
  const double minutes = argc > 2 ? std::atof(argv[2]) : 6.0;
  const uint64_t base_seed = argc > 3 ? static_cast<uint64_t>(std::strtoull(argv[3], nullptr, 10)) : 1;

  Stats total;
  std::vector<float> all_intervals;
  std::vector<float> all_bursts;
  std::vector<float> all_gaze;
  std::vector<float> reaction_ms;
  int distinct_actions_total = 0;

  for (int life = 0; life < lives; ++life) {
    const uint64_t seed = base_seed + static_cast<uint64_t>(life);
    // Every other life runs with a squadmate, so squad behaviour is measured rather
    // than assumed. Solo lives stay solo so a regression in the solo path still shows
    // up against the same numbers as before.
    const Stats s = run_life(seed, minutes, (life % 2) == 1);
    for (const auto& kv : s.action_counts) total.action_counts[kv.first] += kv.second;
    total.time_hurt += s.time_hurt;
    total.time_safe += s.time_safe;
    total.shots += s.shots;
    total.frames += s.frames;
    total.plan_ticks += s.plan_ticks;
    total.committed_ticks += s.committed_ticks;
    total.reaction_held += s.reaction_held;
    total.build_inputs += s.build_inputs;
    all_intervals.insert(all_intervals.end(), s.shot_intervals.begin(), s.shot_intervals.end());
    total.strafe_ticks += s.strafe_ticks;
    total.crouch_ticks += s.crouch_ticks;
    total.crouch_changes += s.crouch_changes;
    total.jumps += s.jumps;
    total.reloads += s.reloads;
    total.dry_ticks += s.dry_ticks;
    total.revives += s.revives;
    total.focus_calls += s.focus_calls;
    total.spread_orders += s.spread_orders;
    total.help_calls += s.help_calls;
    all_gaze.insert(all_gaze.end(), s.gaze_offs.begin(), s.gaze_offs.end());
    total.loot_decisions += s.loot_decisions;
    total.loot_no_target += s.loot_no_target;
    total.loot_walked_to_origin += s.loot_walked_to_origin;
    total.loot_worth_sum += s.loot_worth_sum;
    for (int k = 0; k < 13; ++k) total.loot_kinds[k] += s.loot_kinds[k];
    total.build_pieces += s.build_pieces;
    total.build_edits += s.build_edits;
    total.boxes += s.boxes;
    total.trigger_ticks += s.trigger_ticks;
    total.trigger_windows += s.trigger_windows;
    total.bursts += s.bursts;
    all_bursts.insert(all_bursts.end(), s.burst_lengths.begin(), s.burst_lengths.end());

    // Count how many distinct actions a single bot actually used. A bot with a
    // plan is a bot with a repertoire, not a one-trick loop.
    const int used = static_cast<int>(s.action_counts.size());
    if (used >= 4) ++distinct_actions_total;
  }

  const float denom = static_cast<float>(total.frames);
  std::printf("lives=%d  minutes/life=%.1f  seed=%llu\n", lives, minutes,
              (unsigned long long)base_seed);
  std::printf("frames=%d  bots with >=4 distinct actions: %d/%d\n", total.frames,
              distinct_actions_total, lives);
  std::printf("plan beats: %.1f%%  committed ticks: %.1f%%  held by reaction: %.2f%%\n",
              100.f * static_cast<float>(total.plan_ticks) / denom,
              100.f * static_cast<float>(total.committed_ticks) / denom,
              100.f * static_cast<float>(total.reaction_held) / denom);
  std::printf("shots=%d  trigger duty: %.1f%%  bursts=%d (%.1f/min)  burst length p50=%.2fs p90=%.2fs\n",
              total.shots,
              100.f * static_cast<float>(total.trigger_ticks) /
                  static_cast<float>(std::max(1, total.trigger_windows)),
              total.bursts,
              static_cast<float>(total.bursts) / (static_cast<float>(lives) * static_cast<float>(minutes)),
              pct(all_bursts, 0.50f), pct(all_bursts, 0.90f));
  std::printf("build inputs total=%d (%.2f/s per bot)\n", total.build_inputs,
              static_cast<float>(total.build_inputs) / (denom / 60.f));
  std::printf("time in contact: %.1f%% of life\n",
              100.f * total.time_hurt / (total.time_hurt + total.time_safe));
  if (!all_gaze.empty())
    std::printf("gaze: view off travel p50=%.0f p90=%.0f deg while moving "
                "(p50 wants <20: the bot faces where it walks)\n",
                pct(all_gaze, 0.50f), pct(all_gaze, 0.90f));
  std::printf("squad: revives %d  focus calls %d  spread orders %d  help calls %d\n",
              total.revives, total.focus_calls, total.spread_orders, total.help_calls);
  {
    const int denom_dec = (std::max)(1, total.loot_decisions);
    std::printf("loot: %d decisions  %.0f%% had a target  mean worth %.2f  "
                "no-target %d  walked-at-origin %d\n",
                total.loot_decisions,
                100.f * static_cast<float>(total.loot_decisions - total.loot_no_target) /
                    static_cast<float>(denom_dec),
                static_cast<float>(total.loot_worth_sum) / static_cast<float>(denom_dec),
                total.loot_no_target, total.loot_walked_to_origin);
    std::string kinds;
    for (int k = 0; k < 13; ++k)
      if (total.loot_kinds[k] > 0) {
        kinds += std::string(to_string(static_cast<ItemClass>(k))) + "=" +
                 std::to_string(total.loot_kinds[k]) + " ";
      }
    std::printf("loot mix: %s\n", kinds.c_str());
  }
  std::printf("building: %.2f pieces/s per bot  edits %d  boxes %.2f/min\n",
              static_cast<float>(total.build_pieces) / (denom / 60.f),
              total.build_edits,
              static_cast<float>(total.boxes) /
                  (static_cast<float>(lives) * static_cast<float>(minutes)));
  std::printf("magazine: %.1f reloads/min  reloading %.1f%% of life\n",
              static_cast<float>(total.reloads) /
                  (static_cast<float>(lives) * static_cast<float>(minutes)),
              100.f * static_cast<float>(total.dry_ticks) / denom);
  std::printf("movement: strafing %.1f%% of contact  crouched %.1f%% of life  "
              "crouch changes %.1f/min  jumps %.1f/min\n",
              100.f * static_cast<float>(total.strafe_ticks) /
                  static_cast<float>(std::max(1.f, total.time_hurt / (1.f / 60.f))),
              100.f * static_cast<float>(total.crouch_ticks) / denom,
              static_cast<float>(total.crouch_changes) /
                  (static_cast<float>(lives) * static_cast<float>(minutes)),
              static_cast<float>(total.jumps) /
                  (static_cast<float>(lives) * static_cast<float>(minutes)));

  std::printf("\naction mix:\n");
  std::vector<std::pair<std::string, int>> mix(total.action_counts.begin(), total.action_counts.end());
  std::sort(mix.begin(), mix.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& kv : mix) {
    std::printf("  %-16s %7d  %5.1f%%\n", kv.first.c_str(), kv.second,
                100.f * static_cast<float>(kv.second) / denom);
  }

  // Sanity gates so this is usable as a regression check in CI, not just a toy.
  int failures = 0;
  const float held = 100.f * static_cast<float>(total.reaction_held) / denom;
  if (held > 25.f) {
    std::printf("\nFAIL reaction gate is holding >25%% of ticks (latched bot)\n");
    ++failures;
  }
  if (distinct_actions_total < lives * 3 / 4) {
    std::printf("\nFAIL most bots are one-trick loops\n");
    ++failures;
  }
  if (total.shots == 0) {
    std::printf("\nFAIL bots never fired\n");
    ++failures;
  }
  // A bot that holds the trigger for the whole engagement has no magazine, which
  // you can hear from across the island.
  const float reload_frac = 100.f * static_cast<float>(total.dry_ticks) / denom;
  if (reload_frac < 0.5f || reload_frac > 45.f) {
    std::printf("\nFAIL reloading sits at %.1f%% of life (want 0.5..45%%: either no\n"
                "magazine model or a reload that never finishes)\n",
                reload_frac);
    ++failures;
  }
  // A bot that stands still in a fight is the last obvious tell.
  const float contact_denom = (std::max)(1.f, total.time_hurt / (1.f / 60.f));
  const float strafe_frac = 100.f * static_cast<float>(total.strafe_ticks) / contact_denom;
  if (strafe_frac < 50.f) {
    std::printf("\nFAIL bots strafe only %.1f%% of contact (want >50%%)\n", strafe_frac);
    ++failures;
  }
  // Crouch spam at frame rate is a stutter, not a player. 90 stance changes per
  // bot-minute is roughly the fastest a hand can tap.
  const float crouch_per_min = static_cast<float>(total.crouch_changes) /
                               (static_cast<float>(lives) * static_cast<float>(minutes));
  if (crouch_per_min > 90.f) {
    std::printf("\nFAIL %.1f crouch changes/min (want <90: frame-rate crouch spam)\n",
                crouch_per_min);
    ++failures;
  }
  // Building: a bot that never builds is not playing the game, and a bot that
  // builds every second is a macro. Both are tells, in opposite directions.
  const float build_hz = static_cast<float>(total.build_pieces) / (denom / 60.f);
  if (build_hz < 0.02f || build_hz > 2.f) {
    std::printf("\nFAIL building at %.2f pieces/s per bot (want 0.02..2: either no\n"
                "building or turbo-building)\n", build_hz);
    ++failures;
  }
  // Loot: decisions must exist, mostly have somewhere to go, and never point at the
  // world origin (the (0,0,0) intent bug).
  // Squad behaviour must actually happen in the squad lives (otherwise the layer is
  // inert and the "squad support" claim is empty), and must NOT happen without
  // teammates. Totals here mix solo and pair lives, so assert on presence and
  // plausibility rather than zero.
  if (total.revives == 0 || total.spread_orders == 0) {
    std::printf("\nFAIL squad behaviour never fired in the paired lives "
                "(revives=%d spread=%d)\n", total.revives, total.spread_orders);
    ++failures;
  }
  // A revive is a 14s hold, so a 6 minute life can attempt at most ~25. Far more than
  // that means it re-arms continuously, which is a stuck bot.
  const float revive_per_life = static_cast<float>(total.revives) /
                                (static_cast<float>(lives) * static_cast<float>(minutes));
  if (revive_per_life > 1.f) {
    std::printf("\nFAIL %.2f revive episodes per bot-minute (want <1: revive re-arming "
                "continuously)\n", revive_per_life);
    ++failures;
  }
  if (!all_gaze.empty()) {
    const float gaze50 = pct(all_gaze, 0.50f);
    if (gaze50 > 20.f) {
      std::printf("\nFAIL view is %.0f deg off the travel line at the median (want <20: the\n"
                  "bot walks sideways staring at nothing)\n", gaze50);
      ++failures;
    }
  }
  if (total.loot_decisions == 0) {
    std::printf("\nFAIL bots never decide to loot\n");
    ++failures;
  }
  if (total.loot_walked_to_origin > 0) {
    std::printf("\nFAIL %d loot decisions carried no destination (move_to at the origin)\n",
                total.loot_walked_to_origin);
    ++failures;
  }
  if (total.build_edits == 0) {
    std::printf("\nFAIL no piece is ever edited (a bot walls up and hides, it does\n"
                "not fight from what it built)\n");
    ++failures;
  }
  std::printf("\n%s\n", failures == 0 ? "bench OK" : "bench FAILED");
  return failures == 0 ? 0 : 1;
}
