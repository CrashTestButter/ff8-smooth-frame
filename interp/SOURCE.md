# Source of truth

The files in `interp/` are the FF8 interpolation module: battle, world map, field, and the host
interface `host.h`. **This repository is now their source of truth.** Change them here.

- `interp/README.md` describes the module's architecture and its host contract. It was written in
  the FFNx fork, which is why it talks about `adapter_ffnx.cpp`.
- This repo's host adapter is `src/adapter_standalone.cpp` (ff8interp.dll next to a stock FFNx).
- The FFNx interp fork keeps its own adapter, `adapter_ffnx.cpp`. That file is deliberately not in
  this repo.
- The fork's copy of these files under `FFNx/src/ff8/interp/` is downstream. It will later be
  replaced by a git submodule of this repository. Until then, port changes from here to there, not
  the other way round.

Imported 2026-09-24 from the fork: battle v74, world map v22, field v5, host.h with
`hook_call_chain`.
