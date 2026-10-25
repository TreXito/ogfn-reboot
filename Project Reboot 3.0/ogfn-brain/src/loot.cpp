#include "ogfn/loot.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ogfn {

const char* to_string(ItemClass c) {
  switch (c) {
    case ItemClass::Unknown: return "Unknown";
    case ItemClass::AssaultRifle: return "AR";
    case ItemClass::Shotgun: return "Shotgun";
    case ItemClass::Sniper: return "Sniper";
    case ItemClass::Pistol: return "Pistol";
    case ItemClass::Explosive: return "Explosive";
    case ItemClass::ShieldPotion: return "SmallShield";
    case ItemClass::ShieldBig: return "BigShield";
    case ItemClass::Medkit: return "Medkit";
    case ItemClass::Bandage: return "Bandage";
    case ItemClass::Ammo: return "Ammo";
    case ItemClass::Materials: return "Materials";
    case ItemClass::Trap: return "Trap";
    case ItemClass::Count: return "?";
  }
  return "?";
}

namespace {
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}
}  // namespace

ItemClass classify_item(const std::string& name) {
  const std::string n = lower(name);

  // Order matters. "shotgun" before any generic gun match, "shield" before
  // "potion" because both big and small shields carry Potion in the path, and the
  // size is in the same string.
  // Shields by EVERY spelling the game has used. Shipping only "athena_shield_potion"
  // meant a real 3.5 asset called Athena_ShieldMedium classified as Unknown, and an
  // Unknown pickup is invisible to the valuator: the bot walks past full shield pots.
  if (has(n, "small_shield") || has(n, "smallshield") || has(n, "shieldsmall") ||
      has(n, "shield_small"))
    return ItemClass::ShieldPotion;
  if (has(n, "shield_potion") || has(n, "shieldpotion") || has(n, "shieldmedium") ||
      has(n, "shield_medium") || has(n, "shieldlarge") || has(n, "shield_large") ||
      has(n, "shieldbig") || has(n, "shield_big"))
    return ItemClass::ShieldBig;
  if (has(n, "medkit") || has(n, "med_kit")) return ItemClass::Medkit;
  if (has(n, "bandage")) return ItemClass::Bandage;

  if (has(n, "shotgun")) return ItemClass::Shotgun;
  if (has(n, "sniper")) return ItemClass::Sniper;
  if (has(n, "rocket") || has(n, "grenade") || has(n, "launcher")) return ItemClass::Explosive;
  // "assault" matters more than "rifle": the real 3.5 paths are WID_Assault_Auto_,
  // WID_Assault_Burst_, WID_Assault_Dragon_ and so on, and NOT ONE of them contains
  // the word "rifle". Without this every assault rifle in the game classified as
  // Unknown, so bots never valued one on the floor and never armed from loot.
  if (has(n, "smg") || has(n, "machinegun") || has(n, "machine_gun") ||
      has(n, "rifle") || has(n, "assault"))
    return ItemClass::AssaultRifle;
  if (has(n, "pistol")) return ItemClass::Pistol;

  if (has(n, "ammo")) return ItemClass::Ammo;
  if (has(n, "wood") || has(n, "stone") || has(n, "metal") || has(n, "resource"))
    return ItemClass::Materials;
  if (has(n, "trap")) return ItemClass::Trap;
  return ItemClass::Unknown;
}

float LootValuer::worth_of(const LootSample& s, const Inventory& inv, float dist,
                           float circle_closing_secs, bool engaged) const {
  const Persona& p = persona_;

  // Base worth: what this item does for you RIGHT NOW, given what you carry.
  float base = 0.f;
  switch (s.cls) {
    case ItemClass::ShieldBig:
      // A big pot at 0 shield is the best thing on the floor. At 100 shield it is
      // only worth carrying.
      base = inv.shield < 50 ? 1.00f : (inv.big_shields < 2 ? 0.35f : 0.05f);
      base *= lerpf(1.0f, 1.5f, shield_priority_);
      break;
    case ItemClass::ShieldPotion:
      base = inv.shield < 50 ? 0.75f : (inv.small_shields < 3 ? 0.25f : 0.04f);
      base *= lerpf(1.0f, 1.4f, shield_priority_);
      break;
    case ItemClass::Medkit:
      // A medkit matters when you cannot top off with shields, and much more when
      // this bot is the cautious type.
      base = (inv.medkits < 2 ? 0.55f : 0.10f) * lerpf(0.8f, 1.3f, p.caution);
      break;
    case ItemClass::Bandage:
      base = (inv.bandages < 5 ? 0.22f : 0.03f) * lerpf(0.7f, 1.1f, p.caution);
      break;
    case ItemClass::Shotgun:
      // Shotguns are the close-range currency; a w-keyer values one far more.
      base = inv.has_shotgun ? 0.05f : 0.70f * lerpf(0.8f, 1.4f, p.aggression);
      break;
    case ItemClass::AssaultRifle:
      base = inv.weapon_tier <= 0 ? 0.80f : 0.10f;
      break;
    case ItemClass::Sniper:
      base = inv.has_sniper ? 0.04f : 0.45f * lerpf(1.2f, 0.7f, p.aggression);
      break;
    case ItemClass::Pistol:
      base = inv.weapon_tier <= 0 ? 0.35f : 0.02f;
      break;
    case ItemClass::Explosive:
      base = 0.30f * lerpf(0.8f, 1.3f, p.aggression);
      break;
    case ItemClass::Ammo:
      base = inv.ammo < 60 ? 0.20f : 0.02f;
      break;
    case ItemClass::Materials:
      // Materials feed the build layer: worth real value to a builder, little to a
      // camper who never builds.
      base = 0.25f * lerpf(0.5f, 1.2f, p.build_speed);
      break;
    case ItemClass::Trap:
      base = 0.08f;
      break;
    case ItemClass::Unknown:
      base = 0.03f;  // curiosity, not a plan
      break;
    case ItemClass::Count:
      break;
  }

  // Distance cost. Humans will not walk 40m in the open for a bandage: the cost
  // curve is steep, not linear, and that is what stops bots vacuuming the map.
  //
  // EXCEPT when the item is the thing you cannot play without. An unarmed player
  // crosses a building, a field, whatever it takes, to reach a gun; the same player
  // will not walk 40m for a bandage. One flat curve for both is why an unarmed bot
  // would sit in the open and never arm itself: at 40m the curve charges ~6x, so a
  // rifle scored the same as a bandage two steps away.
  const bool is_weapon = (s.cls == ItemClass::AssaultRifle || s.cls == ItemClass::Shotgun ||
                          s.cls == ItemClass::Sniper || s.cls == ItemClass::Pistol);
  // Same "under-geared keeps shopping" rule as the radius above: the walk cost is
  // gentler for the things a bot is missing, not only for a gun.
  const bool needs_defence = (s.cls == ItemClass::ShieldPotion || s.cls == ItemClass::ShieldBig ||
                              s.cls == ItemClass::Medkit || s.cls == ItemClass::Bandage);
  const bool needy_materials = (s.cls == ItemClass::Materials) &&
                               inv.materials < kMaterialsPerPiece * 3;
  const bool desperate = (inv.weapon_tier <= 0 && is_weapon) ||
                         (needs_defence && inv.shield < 50) || needy_materials;
  const float scale = desperate ? 90.f : 18.f;   // a much gentler falloff when unarmed
  const float walk_cost = 1.f + (dist / scale) * (dist / scale);
  float worth = base / walk_cost;

  // Clock. Loot only competes while there is time; once the circle is closing the
  // same item is worth much less because the walk is the risk.
  const float time_frac = clampf(circle_closing_secs / 45.f, 0.f, 1.f);
  worth *= lerpf(0.25f, 1.f, time_frac);

  // Greed scales how much of this the bot will actually bother with.
  worth *= lerpf(0.6f, 1.4f, p.greed);

  // Mid-fight, going for loot is nearly always wrong. Not zero: a bot that walks
  // past a big pot at 10 shield while a fight is 40m away is also wrong.
  if (engaged) worth *= 0.15f;

  // A tiny deterministic per-pickup bias so two identical items a metre apart do
  // not make the bot dither between them every plan beat. Derived from the id, not
  // drawn, so the same world always produces the same choice (replayable), while
  // different bots still differ through their persona.
  const uint32_t h = static_cast<uint32_t>(s.id) * 2654435761u;
  const float jitter = (static_cast<float>(h % 200u) / 199.f - 0.5f) * 0.06f;
  worth *= 1.f + jitter;
  return worth;
}

LootChoice LootValuer::choose(const std::vector<LootSample>& samples, const Inventory& inv,
                              const Vec3& self, float circle_closing_secs,
                              bool engaged) const {
  LootChoice best;
  for (const auto& s : samples) {
    const float dist = length(s.pos - self);
    // Anything past this radius is a rotation decision, not a loot decision. The one
    // exception is a weapon when you have none: an unarmed player will run a long way
    // to the nearest gun, and a bot that only ever looks 45m ahead stays unarmed for
    // the whole match while slowly rotating to the zone.
    const bool is_weapon = (s.cls == ItemClass::AssaultRifle || s.cls == ItemClass::Shotgun ||
                            s.cls == ItemClass::Sniper || s.cls == ItemClass::Pistol);
    const bool is_defence = (s.cls == ItemClass::ShieldPotion || s.cls == ItemClass::ShieldBig ||
                             s.cls == ItemClass::Medkit || s.cls == ItemClass::Bandage ||
                             s.cls == ItemClass::Ammo);
    // UNDER-GEARED bots keep shopping. A player who has just found a rifle but no
    // shields, no ammo and no materials does not stop looting: they clear the building.
    // Measured in the offline match sim, bots took 1.3 items each and then went quiet for
    // the rest of the game, because once they were "armed" their search radius collapsed
    // to 45m and there was nothing inside it.
    // Materials count as a need too. With finite resources a bot that never picks any up
    // can never build, and the offline sim showed exactly that: 0 materials on every bot
    // and 0 builds in a whole match. Three pieces' worth is the threshold for bothering.
    const bool is_material = (s.cls == ItemClass::Materials);
    const bool under_geared = inv.weapon_tier <= 0 || inv.shield < 50 ||
                              inv.materials < kMaterialsPerPiece * 3;
    const float radius = (under_geared && (is_weapon || is_defence || is_material)) ? 150.f : 45.f;
    if (dist > radius) continue;
    const float w = worth_of(s, inv, dist, circle_closing_secs, engaged);
    if (w > best.worth) {
      best.worth = w;
      best.want = true;
      best.pickup_id = s.id;
      best.pos = s.pos;
      best.cls = s.cls;
    }
  }
  // Floor: below this a human keeps moving instead of veering off. Without it a
  // bot chases 0.01-worth clutter and never rotates.
  if (best.want && best.worth < 0.06f) best.want = false;
  return best;
}

}  // namespace ogfn
