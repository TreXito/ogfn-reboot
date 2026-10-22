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
./sim
