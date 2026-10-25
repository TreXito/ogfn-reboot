#!/usr/bin/env bash
# Compiles the REAL Project Reboot ogfn-brain-adapter.cpp against stub headers
# (copy next to stubs so quoted includes resolve here, not the fork tree), links
# the vendored brain + behavior simulator, and runs the scenarios.
set -euo pipefail
cd "$(dirname "$0")"
ADAPTER="../Project Reboot 3.0/ogfn-brain-adapter.cpp"
BRAIN="../Project Reboot 3.0/ogfn-brain"
cp "$ADAPTER" ./adapter-under-test.cpp
g++ -std=c++20 -DOGFN_SIM_TRACE -DOGFN_HARNESS -Wall -Wextra -Wno-unused-parameter -g \
  -I. -Istubs -I"$BRAIN/include" \
  adapter-under-test.cpp "$BRAIN/src/brain.cpp" "$BRAIN/src/motor.cpp" \
  "$BRAIN/src/perception.cpp" "$BRAIN/src/persona.cpp" "$BRAIN/src/locomotion.cpp" "$BRAIN/src/building.cpp" "$BRAIN/src/loot.cpp" "$BRAIN/src/squad.cpp" "$BRAIN/src/hitmodel.cpp" sim_main.cpp \
  -o ./sim

# WHOLE-MATCH GATE. The scenarios above test pieces in isolation; a piece can pass and
# still compose into nonsense (that is how 50 green assertions coexisted with bots that
# never picked anything up). This runs a full 8-minute match in ~6s and fails if the
# pacing stops being plausible: nobody dying, everyone dying early, nobody armed from
# loot, nobody moving, or walls up nonstop.
# Run it, report it, but do NOT gate on it yet. There is a KNOWN OPEN BUG: long matches
# abort with std::bad_alloc (reproduces on the committed adapter, every run at default
# optimisation, only with many bots over several minutes, and neither ASAN nor UBSAN nor a
# new-handler catches it; no single allocation exceeds 4 MB, so it reads as heap
# corruption). A randomly-red build would be worse than no gate, so the verdict is printed
# and a crash is named, while a clean run is what you want to see.
if ./matchsim 40 8; then
  :
else
  rc=$?
  echo
  echo "MATCH SIM FAILED (rc=$rc): the pieces pass but they do not compose into a match."
  echo "  rc=1 behaviour warnings, rc=2/134 a crash in the sim itself."
  exit 1
fi

# The match simulator: the same real adapter and brain, run as a whole match and
# REPORTED instead of asserted. Separate binary so the assertion suite stays untouched.
g++ -std=c++20 -DOGFN_HARNESS -D_GLIBCXX_ASSERTIONS -Wno-unused-parameter -g \
  -I. -Istubs -I"$BRAIN/include" \
  adapter-under-test.cpp "$BRAIN/src/brain.cpp" "$BRAIN/src/motor.cpp" \
  "$BRAIN/src/perception.cpp" "$BRAIN/src/persona.cpp" "$BRAIN/src/locomotion.cpp" "$BRAIN/src/building.cpp" "$BRAIN/src/loot.cpp" "$BRAIN/src/squad.cpp" "$BRAIN/src/hitmodel.cpp" match_sim.cpp \
  -o ./matchsim

./sim

# WHOLE-MATCH GATE. The scenarios above test pieces in isolation; a piece can pass and
# still compose into nonsense (that is how 50 green assertions coexisted with bots that
# never picked anything up). This runs a full 8-minute match in ~6s and fails if the
# pacing stops being plausible: nobody dying, everyone dying early, nobody armed from
# loot, nobody moving, or walls up nonstop.
# Run it, report it, but do NOT gate on it yet. There is a KNOWN OPEN BUG: long matches
# abort with std::bad_alloc (reproduces on the committed adapter, every run at default
# optimisation, only with many bots over several minutes, and neither ASAN nor UBSAN nor a
# new-handler catches it; no single allocation exceeds 4 MB, so it reads as heap
# corruption). A randomly-red build would be worse than no gate, so the verdict is printed
# and a crash is named, while a clean run is what you want to see.
if ./matchsim 40 8; then
  :
else
  rc=$?
  echo
  echo "MATCH SIM FAILED (rc=$rc): the pieces pass but they do not compose into a match."
  echo "  rc=1 behaviour warnings, rc=2/134 a crash in the sim itself."
  exit 1
fi
