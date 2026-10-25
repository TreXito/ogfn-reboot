#pragma once

#include <utility>
#include <vector>

#include "mathx.h"
#include "persona.h"
#include "rng.h"

namespace ogfn {

// Wood cost of one building piece, in the game's units. Lives here because the brain has
// to know what a build COSTS before it plans one: scoring Build with an empty material
// pile produced refused inputs, i.e. a bot repeatedly asking for something impossible.
inline constexpr int kMaterialsPerPiece = 10;


// Which piece the bot wants next. Ramp first is the human habit: you want height
// before anything else, because height wins every trade that is not point blank.
enum class PieceKind { None, Wall, Ramp, Floor, Cone };
const char* to_string(PieceKind p);

// One build input for the engine to execute this tick. Placed pieces are resolved
// by the engine against its own grid, so this carries the intent (what, which way,
// and whether it is an edit of the piece already there) rather than coordinates.
struct BuildInput {
  PieceKind piece = PieceKind::None;
  float yaw_deg = 0.f;      // facing the piece is placed against
  bool edit = false;        // converting the existing piece instead of placing
  bool reset = false;       // human habit: reset the build selection between sets
};

// Why a piece was refused by the world, which is the only feedback the builder
// gets and the reason it must not keep swinging at a wall it cannot place.
enum class BuildRefusal {
  None,
  NoResource,     // out of wood/brick/steel
  Blocked,        // the cell is occupied or the surface is not buildable
  OutOfRange,     // too far, or inside the player
  Unknown,
};

// Why a bot boxes up, in the order humans do it.
enum class BuildGoal {
  None,
  // Taking fire and losing the trade: put walls between us now.
  BoxUp,
  // Ahead of a push: ramp to height, then wall at the top.
  GainHeight,
  // Wants to shoot from cover: box, then edit a window instead of stepping out.
  PeekWindow,
};

// Where a piece goes in world space. This lives in the brain rather than the
// engine adapter for one reason: grid alignment is easy to get subtly wrong and
// almost invisible in a log. A wall half a cell off looks fine in a screenshot and
// acts as a gap in a box, so the arithmetic is pinned by a test instead.
//
// Meters here; the adapter converts to uu at the boundary and is the only code
// that knows about uu.
struct BuildPiece {
  Vec3 pos;             // centre of the piece
  float yaw_deg = 0.f;  // the piece's own orientation
  bool valid = false;
};

struct BuildGrid {
  float cell = 5.12f;         // meters per cell (512 uu)
  float wall_height = 3.84f;  // meters per wall tier (384 uu)

  // Piece for a player standing at `self` (feet) facing `yaw_deg`, for `step` of a
  // sequence: a box fans around the player instead of stacking in one plane.
  BuildPiece place(PieceKind kind, const Vec3& self, float yaw_deg, int step) const;
};

// The building tell. Three things make a bot's building obvious, and this layer is
// built around each:
//   1. Speed. Turbo-building at input rate is machine behaviour. Humans place at
//      2 to 7 inputs per second and they miss inputs.
//   2. Sequence. A bot that places one wall and stands there is not building, it
//      is hiding. A human boxes up (four sides plus a roof) or ramps up.
//   3. Using the piece. Placing a wall and then walking around it is what a bot
//      does. A human edits a window and shoots through it, which is why the peek
//      goal exists at all.
class Builder {
 public:
  Builder(const Persona& p, uint64_t seed) : persona_(p), rng_(seed) {
    // Faster builders commit to a plan sooner and hold it; slow builders dither,
    // place a wall, and change their mind. Both read as human.
    plan_hold_secs_ = lerpf(2.6f, 1.3f, p.build_speed);
  }

  // The engine reports the outcome of the last input it was given. Optional: a
  // harness or a sim that never refuses anything can ignore it entirely, and the
  // builder then behaves exactly as it did before.
  //
  // `repeat` means the same input was refused again, which is what turns a one-off
  // mistake into evidence that the plan is wrong rather than unlucky.
  void report_result(bool placed, BuildRefusal why);

  // Called every tick. Returns the single input to send this tick, or None.
  //
  // `taking_fire` should be true while damage is actually incoming. `wants_cover`
  // is the brain's Build action. `height_available` false (indoors, in a cave)
  // means the ramp sequence is skipped rather than spammed at a ceiling.
  BuildInput update(float dt, bool wants_cover, bool taking_fire, float health_frac,
                    float yaw_to_threat_deg, bool height_available);

  BuildGoal goal() const { return goal_; }
  // Pieces placed since the current goal started. The engine and the bench both
  // read this; "bot boxes up" is 4 walls + 1 roof, "bot ramps" is 2 to 4 pieces.
  int placed_in_goal() const { return placed_in_goal_; }
  int total_placed() const { return total_placed_; }
  // Refusals seen, and how many plans were abandoned because of them. A builder that
  // never learns its piece was refused keeps re-sending the same impossible input,
  // which at 2 to 7 inputs a second is the loudest macro tell there is.
  int refusals() const { return refusals_; }
  int plans_abandoned() const { return plans_abandoned_; }
  bool out_of_material() const { return out_of_material_; }
  int total_edits() const { return total_edits_; }
  int reset_count() const { return resets_; }

  // Inputs per second this bot can physically sustain. Exposed so a test can pin
  // it inside the human band instead of trusting the constants.
  float input_hz() const { return lerpf(2.0f, 7.0f, persona_.build_speed); }

 private:
  void start_goal(BuildGoal g, float now);

  Persona persona_;
  Rng rng_;

  BuildGoal goal_ = BuildGoal::None;
  float plan_hold_secs_ = 2.f;
  double goal_started_ = 0.0;
  double now_ = 0.0;

  int step_ = 0;           // index into the current goal's sequence
  int placed_in_goal_ = 0;
  int total_placed_ = 0;
  int total_edits_ = 0;
  int resets_ = 0;

  // Use-what-you-built clocks. A plan that just finished is on cooldown so the bot
  // fights from the piece instead of instantly rebuilding it.
  double box_ready_ = 0.0;
  double peek_ready_ = 0.0;
  double push_ready_ = 0.0;

  // Feedback state.
  int refusals_ = 0;
  int plans_abandoned_ = 0;
  int consecutive_refusals_ = 0;
  BuildRefusal last_refusal_ = BuildRefusal::None;
  bool out_of_material_ = false;
  double material_hold_until_ = 0.0;   // staying off building while the counter is empty
  double abandon_hold_until_ = 0.0;    // beat of doing something else after giving up

  float accum_ = 0.f;      // input accumulator, so the cadence is honest
  float base_yaw_ = 0.f;   // heading the sequence was started at
  bool last_was_edit_ = false;

  // The concrete input sequence for the current goal: (piece, is_edit). A human
  // plan is a list of specific pieces, not a vague intention, and each goal has
  // exactly one sequence it is working through.
  std::vector<std::pair<PieceKind, bool>> plan_;
  int sequence_len_ = 0;
};

}  // namespace ogfn
