# Vendored ogfn brain

Snapshot of ~/ogfn/bots (engine-agnostic C++ bot brain), copied into the gameserver
tree so MSVC compiles it beside the fork. Canonical source with tests lives OUT of
this tree: run `make test` in ~/ogfn/bots (4037 checks, 0 failures). After editing
there, re-sync with `cp` of include/ + src/*.cpp and never edit files here directly.
