#pragma once
#include <string>
#include <vector>

#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// What a pickup actually is, in the only granularity that changes a decision.
// A bot does not care that a thing is "rare"; it cares that it is a shield potion
// and it is out of potions.
enum class ItemClass {
  Unknown = 0,
  AssaultRifle,
  Shotgun,
  Sniper,
  Pistol,
  Explosive,
  ShieldPotion,   // small shield: +25
  ShieldBig,      // big pot: +50
  Medkit,
  Bandage,
  Ammo,
  Materials,
  Trap,
  Count,
};
const char* to_string(ItemClass c);

// One pickup observed in the world, as the engine sees it.
struct LootSample {
  int id = 0;
  Vec3 pos;
  ItemClass cls = ItemClass::Unknown;
  int count = 1;
};

// What the bot already has. This is what turns looting from "walk to the nearest
// thing" into a valuation problem, and it is the reason a bot does not pick up a
// fourth grey pistol while it has no shields.
struct Inventory {
  int weapon_tier = 0;      // best gun it is holding: 0 none, 1 grey .. 5 gold
  bool has_shotgun = false;
  bool has_sniper = false;
  int shield = 0;           // current shield points, 0..100
  int small_shields = 0;    // carried
  int big_shields = 0;
  int medkits = 0;
  int bandages = 0;
  int ammo = 0;
  int materials = 0;
};

// A loot decision: which pickup to walk to, and why. `worth` is comparable across
// samples so the brain can rank, and `want` false means "nothing here is worth the
// walk", which is what stops a bot from looting a pile of bandages at full health
// in the open with 20 seconds of storm left.
struct LootChoice {
  bool want = false;
  int pickup_id = 0;
  Vec3 pos;
  float worth = 0.f;
  ItemClass cls = ItemClass::Unknown;
};

// Loot valuation. The thesis: loot choice is a function of what you already have,
// how much of the clock is left, and where the thing is. Two bots with the same
// inventory disagree because their personas weight it differently (a greedy bot
// walks 30m for a big pot, a cautious one does not cross open ground).
class LootValuer {
 public:
  LootValuer(const Persona& p, uint64_t seed) : persona_(p), rng_(seed) {
    // Some players always top off shields first, others grab a better gun first.
    // Fixed per bot, so a squad does not all loot in the same order.
    shield_priority_ = clampf(0.55f + rng_.normal(0.f, 0.18f), 0.2f, 0.95f);
  }

  // Rank the visible samples and pick what to walk to. `circle_closing_secs` is
  // how long until the zone forces a move: with little time left, loot further
  // than a short run is not worth it, and that alone reads as human.
  LootChoice choose(const std::vector<LootSample>& samples, const Inventory& inv,
                    const Vec3& self, float circle_closing_secs, bool engaged) const;

  // Worth of a single pickup in this bot's current situation. Exposed because the
  // tests assert orderings (a big pot beats an ammo box when shields are down)
  // rather than exact numbers.
  float worth_of(const LootSample& s, const Inventory& inv, float dist,
                 float circle_closing_secs, bool engaged) const;

 private:
  Persona persona_;
  Rng rng_;
  float shield_priority_ = 0.6f;
};

// Map an item definition path/name to a class. The engine hands over asset names,
// not enums, and the names differ per era, so this matches on substrings in a
// deliberate order (specific before generic: "Shotgun" before "Gun").
ItemClass classify_item(const std::string& name);

}  // namespace ogfn
