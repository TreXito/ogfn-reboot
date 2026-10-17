#pragma once
#include <cstdint>

namespace ogfn {

// Number of entries in Persona::bias, keeps it in step with enum ActionKind.
constexpr int kActionCount = 9;

// A bot's skill is a point in a population distribution, not a difficulty slider.
// Lobbies are filled by sampling this, so some bots are genuinely bad and a few
// are genuinely good, which is what a real lobby looks like.
struct Persona {
  float aggression = 0.5f;    // pushes fights it could avoid
  float caution = 0.5f;       // retreats earlier, heals sooner
  float patience = 0.5f;      // willingness to hold an angle instead of rotating
  float greed = 0.5f;         // loots longer before moving to the circle
  float build_speed = 0.5f;   // inputs per second skill
  float aim_skill = 0.5f;     // servo gain / overshoot quality
  float reaction_ms = 230.f;  // median reaction latency
  float reaction_sigma = 0.35f;

  // Stable idiosyncrasy per action, drawn once per bot. Humans are consistently
  // a bit weird in the same way (always peeks, never loots crates), and this is
  // what stops a lobby of identical utility functions from marching in lockstep.
  // 1.0 is neutral.
  float bias[kActionCount] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};

  static Persona sample(class Rng& rng, float skill_bias = 0.f);
};

// Hand-authored lobby archetypes, neutral bias, for tuning and tests.
Persona persona_w_keyer();
Persona persona_camper();
Persona persona_rotator();
Persona persona_third_party();

}  // namespace ogfn
