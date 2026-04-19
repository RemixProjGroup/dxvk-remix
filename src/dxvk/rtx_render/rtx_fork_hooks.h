#pragma once

// rtx_fork_hooks.h — declarations for the fork-owned hook functions that
// upstream files call into. Each hook's implementation lives in a dedicated
// rtx_fork_*.cpp file, keeping upstream files' fork footprint to one-line
// call sites only.
//
// See docs/fork-touchpoints.md for the index of every hook and which
// upstream file calls it.

namespace fork_hooks {

  // Hook declarations are added here during Phase 3 migration, as each
  // upstream file's inline fork logic is lifted into a fork-owned
  // implementation file.

}
