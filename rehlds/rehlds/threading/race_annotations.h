#pragma once

// Happens-before annotations for race detectors that do not understand C++11
// atomics (Helgrind, DRD). Built in with -DREHLDS_HELGRIND=ON (CMake), empty
// otherwise. See tools/bench/race_check.sh.
#ifdef REHLDS_HELGRIND
#include <valgrind/helgrind.h>

// Before a release store that publishes data
#define RACE_HAPPENS_BEFORE(addr) ANNOTATE_HAPPENS_BEFORE(addr)
// After an acquire load that observed the published data
#define RACE_HAPPENS_AFTER(addr) ANNOTATE_HAPPENS_AFTER(addr)
// Statistics read without synchronisation on purpose
#define RACE_BENIGN(addr, size, reason) ANNOTATE_BENIGN_RACE_SIZED(addr, size, reason)
#else
#define RACE_HAPPENS_BEFORE(addr)
#define RACE_HAPPENS_AFTER(addr)
#define RACE_BENIGN(addr, size, reason)
#endif
