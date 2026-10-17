#include "ogfn/persona.h"

#include "ogfn/mathx.h"
#include "ogfn/rng.h"

namespace ogfn {

namespace {
float jitter(Rng& rng, float base, float spread) {
  return clampf(base + rng.normal(0.f, spread), 0.f, 1.f);
}
}  // namespace

Persona Persona::sample(Rng& rng, float skill_bias) {
  Persona p;
  p.aggression = rng.unit() < 0.30f ? 0.85f : (rng.unit() < 0.50f ? 0.55f : 0.25f);
  p.caution = jitter(rng, 0.5f, 0.22f);
  p.patience = jitter(rng, 0.45f, 0.25f);
  p.greed = jitter(rng, 0.5f, 0.25f);

  // Skill rises with the bias; spread keeps a few outliers at both ends.
  const float s = clampf(0.5f + skill_bias, 0.05f, 0.95f);
  p.build_speed = clampf(s + rng.normal(0.f, 0.18f), 0.05f, 0.99f);
  p.aim_skill = clampf(s + rng.normal(0.f, 0.18f), 0.05f, 0.99f);

  // Better players react faster, but the tail stays heavy: even good humans
  // have slow moments, so sigma shrinks less than the median does.
  p.reaction_ms = clampf(rng.lognormal_median(lerpf(320.f, 180.f, s), 0.30f), 90.f, 900.f);
  p.reaction_sigma = 0.35f;

  // Idiosyncrasies: fixed per bot, so two equally skilled bots still play
  // differently and predictably so.
  for (int i = 0; i < kActionCount; ++i) {
    p.bias[i] = clampf(1.f + rng.normal(0.f, 0.18f), 0.55f, 1.5f);
  }
  return p;
}

Persona persona_w_keyer() {
  Persona p;
  p.aggression = 0.9f; p.caution = 0.2f; p.patience = 0.2f; p.greed = 0.35f;
  p.build_speed = 0.65f; p.aim_skill = 0.6f; p.reaction_ms = 200.f;
  return p;
}

Persona persona_camper() {
  Persona p;
  p.aggression = 0.15f; p.caution = 0.8f; p.patience = 0.85f; p.greed = 0.6f;
  p.build_speed = 0.45f; p.aim_skill = 0.5f; p.reaction_ms = 260.f;
  return p;
}

Persona persona_rotator() {
  Persona p;
  p.aggression = 0.4f; p.caution = 0.6f; p.patience = 0.3f; p.greed = 0.4f;
  p.build_speed = 0.7f; p.aim_skill = 0.65f; p.reaction_ms = 210.f;
  return p;
}

Persona persona_third_party() {
  Persona p;
  p.aggression = 0.7f; p.caution = 0.5f; p.patience = 0.8f; p.greed = 0.45f;
  p.build_speed = 0.55f; p.aim_skill = 0.55f; p.reaction_ms = 240.f;
  return p;
}

}  // namespace ogfn
