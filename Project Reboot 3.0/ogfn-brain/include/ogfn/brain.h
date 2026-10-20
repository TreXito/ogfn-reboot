#pragma once
#include <functional>
#include <string>
#include <vector>

#include "loot.h"
#include "mathx.h"
#include "perception.h"
#include "persona.h"
#include "rng.h"
#include "squad.h"

namespace ogfn {

enum class ActionKind {
  Idle,
  Loot,
  RotateToCircle,
  Engage,
  Build,
  Heal,
  Flee,
  ThirdParty,
  HoldAngle,
  // Walk to a downed squadmate. Its own action rather than a disguised rotation so
  // telemetry and the bench can see revives as revives.
  Revive,
};

const char* to_string(ActionKind k);

struct WorldSnapshot {
  double time = 0.0;
  Vec3 self_pos;
  float self_health = 100.f;
  float self_shield = 100.f;
  float self_yaw_deg = 0.f;
  bool has_weapon = true;
  int shield_potions = 0;
  float circle_center_x = 0.f, circle_center_y = 0.f;
  float circle_next_center_x = 0.f, circle_next_center_y = 0.f;
  float circle_radius = 200.f;
  float circle_shrink_in_secs = 120.f;
  float distance_to_circle_edge = 0.f;   // negative inside safe zone
  int loot_within_30m = 0;
  int enemies_within_30m = 0;
  int enemies_currently_fighting_each_other = 0;
  bool target_is_visible = false;
  float target_distance = 0.f;
  float target_health = 100.f;
  bool target_is_reloading = true;

  // Loot, as objects rather than a count. `loot_within_30m` is still here because
  // the score reads it as "is there anything here at all", but the DECISION uses
  // the samples: which pickup, and is it worth crossing the ground for. A count
  // cannot tell a big pot from an ammo box, and that difference is the whole of
  // loot decision making.
  std::vector<LootSample> loot;
  Inventory inventory;
  // Where the fight the bot is third-partying is happening (used by ThirdParty).
  float fight_x = 0.f, fight_y = 0.f;

  // Consumables. Healing is a timed action in this game, not a click: the bot that
  // decides "heal" is committed to a 2-3s animation it can be shot out of. Without
  // this, Heal was a per-tick decision with no duration, so a bot "healed" instantly
  // and could re-decide the next tick, which looks nothing like a player chugging a
  // potion behind a wall.
  int big_shields = 0;     // +50 each
  int small_shields = 0;   // +25 each
  int medkits = 0;         // +100 hp each
  int bandages = 0;        // +15 hp each, capped
  // Seconds left on a heal in progress, and which item it is consuming. The engine
  // sets these while the animation runs; the brain must not start another action.
  float heal_remaining_secs = 0.f;
  int healing_item = 0;    // 0 none, 1 small shield, 2 big shield, 3 medkit, 4 bandage

  // Squad context. Empty (or all solo) means the old solo behaviour exactly, so a
  // solo lobby is unchanged and only real squads get squad decisions.
  std::vector<Teammate> teammates;
  bool enemies_pushing_me = false;   // taking damage this moment
  uint64_t now_ms = 0;               // monotonic clock for call cooldowns
  // Legs. Where the bot is travelling and how far, when it is travelling: the brain
  // needs this to decide where to LOOK, and looking where you walk is one of the most
  // basic human tells. Optional: zeros mean "not travelling".
  Vec3 travel_to;
  bool travelling = false;
};

struct Decision {
  ActionKind action = ActionKind::Idle;
  float score = 0.f;
  Vec3 move_to;
  bool fire = false;
  bool build_wall = false;
  // Where the player is LOOKING as distinct from where they are going. A human faces
  // their travel direction out of combat and their target inside one; the engine
  // blends the camera toward this when face_travel is set. Without it a bot walks
  // sideways with its crosshair on nothing.
  float look_yaw_deg = 0.f;
  bool face_travel = false;
  std::string reason;

  // All candidate scores, sorted, kept for tuning and tests.
  std::vector<std::pair<std::string, float>> scores;
};

// Utility AI with human cadence.
//
// Two ideas do the work here:
//   1. Humans do not re-plan at 60Hz. They commit to an intent for a beat (the
//      plan interval, roughly 8Hz), which removes the frame-to-frame argmax
//      flicker that makes naive utility AI look robotic.
//   2. A surprise (combat involvement changing) costs reaction time. Everything
//      else is adopted at the next plan beat.
//
// The result degrades gracefully: there is no state machine to get stuck in,
// only scores that shift as the world does.
class Brain {
 public:
  Brain(Persona p, uint64_t seed)
      : persona_(p), rng_(seed), loot_(p, seed ^ 0x10077ull), squad_(seed ^ 0x5C0ADull) {
    // A fixed rotation side: the ring entry angle this bot prefers for the whole match.
    Rng habit(seed ^ 0x2074A710Bull);
    rotation_offset_rad_ = habit.range(0.f, 2.f * kPi);
    // Re-plan cadence: impatient bots re-evaluate quicker, patient ones settle.
    plan_fast_secs_ = lerpf(0.09f, 0.06f, p.patience);
    plan_slow_secs_ = lerpf(0.24f, 0.12f, p.patience);
  }

  const Persona& persona() const { return persona_; }
  const LootValuer& loot_valuer() const { return loot_; }
  // True while a heal animation the brain committed to is still running. Exposed so
  // the engine can hold the bot in it and the bench can measure heal uptime.
  bool healing() const { return heal_left_ > 0.f; }
  int heal_item() const { return heal_item_; }
  // Seconds left on the committed heal, so the engine can animate and count against
  // the same number the brain decided instead of inventing its own.
  float heal_seconds_left() const { return heal_left_ > 0.f ? heal_left_ : 0.f; }
  const SquadBrain& squad() const { return squad_; }
  const SquadDirective& squad_directive() const { return squad_directive_; }
  // The view direction the movement layer asked for, if any. The engine blends the
  // camera toward this out of combat and ignores it during a fight.
  bool gaze_active() const { return gaze_active_; }
  float gaze_yaw_deg() const { return gaze_yaw_deg_; }
  void set_gaze(bool active, float yaw_deg) { gaze_active_ = active; gaze_yaw_deg_ = yaw_deg; }

  // What the bot decided to walk to. Persists across committed ticks (the valuation
  // only re-runs on a plan beat), and is empty when the last Loot plan found
  // nothing worth the walk.
  const LootChoice& loot_choice() const { return loot_choice_; }
  Perception& perception() { return perception_; }
  const Perception& perception() const { return perception_; }
  void set_perception_config(PerceptionConfig cfg) { perception_ = Perception(cfg); }

  // Reaction time for a surprise event, sampled per event so it never averages
  // out into machine-perfect consistency.
  float sample_reaction_secs() {
    return rng_.lognormal_median(persona_.reaction_ms / 1000.f, persona_.reaction_sigma);
  }

  Decision think(const WorldSnapshot& w);
  void observe(const Observation& obs, double now) { perception_.update(obs, now); }

  bool is_reacting(double now) const { return in_reaction_ && now < reaction_deadline_; }
  double pending_until() const { return reaction_deadline_; }

  // Test/tuning hook: pretend the bot was already doing `a` as of `now`.
  void prime_action(ActionKind a, double now) {
    last_action_ = a;
    has_decided_ = true;
    in_reaction_ = false;
    engaged_ = false;
    reaction_deadline_ = now;
    next_plan_time_ = now;
  }

 private:
  float score(ActionKind k, const WorldSnapshot& w) const;
  Decision make_intent(ActionKind a, const WorldSnapshot& w);

  Persona persona_;
  Rng rng_;
  Perception perception_;
  // Where the view should be, per the movement layer (see Locomotion::MoveOutput).
  float gaze_yaw_deg_ = 0.f;
  bool gaze_active_ = false;
  // The gaze that was PLANNED at the last plan beat. Committed ticks reuse it instead
  // of re-deriving one from a move_to that keeps moving.
  float last_look_yaw_ = 0.f;
  bool last_face_travel_ = false;
  // Committed heal: which item, and how long is left.
  float heal_left_ = 0.f;
  int heal_item_ = 0;

  // Fixed per bot: which side of the ring it prefers to rotate in on. Derived once
  // from the seed so a bot's rotation line is a habit, not a per-tick coin flip.
  float rotation_offset_rad_ = 0.f;
  LootValuer loot_;
  LootChoice loot_choice_;
  SquadBrain squad_;
  SquadDirective squad_directive_;

  float plan_fast_secs_ = 0.08f;
  float plan_slow_secs_ = 0.20f;

  double next_plan_time_ = -1e9;
  bool has_decided_ = false;
  ActionKind last_action_ = ActionKind::Idle;
  float last_score_ = 0.f;

  bool engaged_ = false;          // bot's own view of being in a fight
  bool in_reaction_ = false;      // one reaction episode, never extended
  double reaction_deadline_ = -1e9;
};

}  // namespace ogfn
