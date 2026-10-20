#include "ogfn/building.h"

#include "ogfn/mathx.h"

namespace ogfn {

const char* to_string(PieceKind p) {
  switch (p) {
    case PieceKind::None: return "None";
    case PieceKind::Wall: return "Wall";
    case PieceKind::Ramp: return "Ramp";
    case PieceKind::Floor: return "Floor";
    case PieceKind::Cone: return "Cone";
  }
  return "?";
}

namespace {
// Fall back to the ground if nothing happens for this long: a bot that keeps
// its build plan through a whole rotation is not reacting to the world.
constexpr double kGoalTimeout = 6.0;
// After a completed box, a human sits in it and shoots out of the window. They do
// not rebuild the same box every two seconds: without this cooldown a bot under
// sustained fire placed 150 pieces in 30 seconds, which is a macro, not a player.
constexpr double kBoxCooldown = 5.0;
constexpr double kPeekCooldown = 1.6;
}  // namespace

BuildPiece BuildGrid::place(PieceKind kind, const Vec3& self, float yaw_deg, int step) const {
  BuildPiece out;
  if (kind == PieceKind::None) return out;

  // A box is walked around one quarter turn at a time; a ramp line all goes the
  // same way, which is why only Wall fans with the step.
  const float yaw = (kind == PieceKind::Wall) ? wrap180(yaw_deg + 90.f * static_cast<float>(step))
                                              : yaw_deg;
  const Vec3 fwd = yaw_to_dir(yaw);

  switch (kind) {
    case PieceKind::Wall:
      // Walls sit one half-cell out from the player's feet, centred on the edge of
      // the cell they occupy.
      out.pos = Vec3(self.x + fwd.x * cell * 0.5f, self.y + fwd.y * cell * 0.5f, self.z);
      break;
    case PieceKind::Floor:
      // A roof is a cell above the head, aligned to the player's own cell.
      out.pos = Vec3(self.x, self.y, self.z + wall_height);
      break;
    case PieceKind::Ramp:
      // Ramps occupy the cell in front and climb one wall tier over one cell.
      out.pos = Vec3(self.x + fwd.x * cell, self.y + fwd.y * cell, self.z);
      break;
    case PieceKind::Cone:
      out.pos = Vec3(self.x, self.y, self.z + wall_height);
      break;
    case PieceKind::None:
      return out;
  }
  out.yaw_deg = yaw;
  out.valid = true;
  return out;
}

void Builder::start_goal(BuildGoal g, float now) {
  goal_ = g;
  goal_started_ = now;
  step_ = 0;
  placed_in_goal_ = 0;
  base_yaw_ = 0.f;  // set on the first input, which is when we know the threat dir
  accum_ = 0.f;
  plan_.clear();
  switch (g) {
    case BuildGoal::BoxUp:
      // Three walls around, an EDITED window in the one facing the threat, the
      // closing wall, then a roof. Boxing up and then not being able to shoot is
      // a coffin, not cover: the window edit is what makes the box a position.
      plan_ = {{PieceKind::Wall, false},
               {PieceKind::Wall, false},
               {PieceKind::Wall, false},
               {PieceKind::Wall, true},   // edit the front wall: window
               {PieceKind::Floor, false}  // roof
      };
      break;
    case BuildGoal::PeekWindow:
      // One wall, then a window in it.
      plan_ = {{PieceKind::Wall, false}, {PieceKind::Wall, true}};
      break;
    case BuildGoal::GainHeight: {
      // Covered ramp push: ramp, wall, ramp, wall. Two to four ramps, sampled
      // once per plan because how far the ground above them is is unknown.
      const int ramps = rng_.int_range(2, 4);
      for (int i = 0; i < ramps; ++i) {
        plan_.push_back({PieceKind::Ramp, false});
        plan_.push_back({PieceKind::Wall, false});
      }
      break;
    }
    case BuildGoal::None:
      break;
  }
  sequence_len_ = static_cast<int>(plan_.size());
}

BuildInput Builder::update(float dt, bool wants_cover, bool taking_fire, float health_frac,
                           float yaw_to_threat_deg, bool height_available) {
  BuildInput out;
  if (dt <= 0.f) return out;
  now_ += dt;

  // --- goal selection -------------------------------------------------------
  // Being shot at is the strongest signal there is. Losing the trade turns it
  // into a box rather than cover: you want walls on all sides, not one.
  const bool hurt = health_frac < 0.55f;
  const bool losing = health_frac < 0.35f;

  BuildGoal want = BuildGoal::None;
  if (taking_fire || wants_cover) {
    if (losing || (taking_fire && hurt)) want = BuildGoal::BoxUp;
    else if (height_available && rng_.unit() < 0.5f) want = BuildGoal::GainHeight;
    else want = BuildGoal::PeekWindow;
  }

  // Cooldown gates: a finished plan blocks the same plan from restarting until the
  // bot has actually used it. Being shot harder overrides the box cooldown via the
  // BoxUp branch below, because getting pushed is a real reason to rebuild.
  if (want == BuildGoal::PeekWindow && now_ < peek_ready_) want = BuildGoal::None;
  if (want == BuildGoal::GainHeight && now_ < push_ready_) want = BuildGoal::None;

  const bool plan_expired = goal_ != BuildGoal::None && now_ - goal_started_ > kGoalTimeout;
  const bool box_cooling = want == BuildGoal::BoxUp && now_ < box_ready_;
  if (!box_cooling && want != goal_ &&
      (goal_ == BuildGoal::None || plan_expired || want == BuildGoal::BoxUp)) {
    // A human mid-sequence does not restart for a weaker signal, but being shot
    // does interrupt anything: that is the one case worth dropping the plan for.
    if (want != BuildGoal::None) start_goal(want, static_cast<float>(now_));
  } else if (want == BuildGoal::None && goal_ != BuildGoal::None) {
    // Out of danger, but finish the piece you already started. Abandoning a
    // half-built box is a tell; players complete the wall they are on.
    if (placed_in_goal_ >= sequence_len_) {
      goal_ = BuildGoal::None;
      step_ = 0;
      placed_in_goal_ = 0;
    }
  }

  if (goal_ == BuildGoal::None) return out;

  // --- cadence --------------------------------------------------------------
  // Inputs come at this bot's sustainable rate, not once per frame. Missing an
  // input is normal and is what separates "fast builder" from "macro".
  accum_ += input_hz() * dt;
  if (accum_ < 1.f) return out;
  accum_ -= 1.f;

  // Missed input: the piece does not get placed this beat. Worse builders miss
  // more, and a miss costs the beat, not the plan.
  if (rng_.unit() < lerpf(0.16f, 0.04f, persona_.build_speed)) return out;

  // --- the input itself ------------------------------------------------------
  if (step_ >= sequence_len_) {
    goal_ = BuildGoal::None;
    step_ = 0;
    placed_in_goal_ = 0;
    return out;
  }
  const PieceKind piece = plan_[static_cast<size_t>(step_)].first;
  const bool is_edit = plan_[static_cast<size_t>(step_)].second;
  out.piece = piece;
  // First input of a plan anchors on the threat; the rest of the box fans 90
  // degrees per wall so four pieces go around, not all in one plane.
  if (step_ == 0)
    base_yaw_ = yaw_to_threat_deg;
  out.yaw_deg = wrap180(base_yaw_ + static_cast<float>(step_) * 90.f);

  // Placing a wall and then walking around it is what a bot does. A human edits a
  // window and shoots through it, which is the whole point of building in a fight.
  out.edit = is_edit;
  if (is_edit) {
    ++total_edits_;
  } else {
    ++total_placed_;
  }
  last_was_edit_ = is_edit;
  ++placed_in_goal_;

  // Between sets humans reset the selection, which is also what makes the next
  // piece go where they want. Not every piece, every few.
  if (step_ == 0 && rng_.unit() < 0.5f) {
    out.reset = true;
    ++resets_;
  }

  ++step_;
  // A finished plan puts its own kind on cooldown, so the bot uses what it built
  // instead of rebuilding it. Placing is not the goal; shooting from it is.
  if (step_ >= sequence_len_) {
    switch (goal_) {
      case BuildGoal::BoxUp: box_ready_ = now_ + kBoxCooldown; break;
      case BuildGoal::PeekWindow: peek_ready_ = now_ + kPeekCooldown; break;
      case BuildGoal::GainHeight: push_ready_ = now_ + kBoxCooldown; break;
      case BuildGoal::None: break;
    }
    goal_ = BuildGoal::None;
    step_ = 0;
    placed_in_goal_ = 0;
  }
  return out;
}

}  // namespace ogfn
