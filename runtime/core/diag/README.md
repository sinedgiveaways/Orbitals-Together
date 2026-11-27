# runtime/core/diag

Diagnostics: flight recorder, counters and profiling, RenderDoc, game phases and the census of SDK calls.

| File | What it is |
|---|---|
| `flight.c`, `flight.h`, `perf.c`, `prof*`, `perfmon.*`, `rdoc.c` | Diagnostics: flight recorder, counters, profiling, RenderDoc |
| `impcensus.c` | `ORBITALS_IMPORT_CENSUS=<file>`: which SDK functions the game code calls during the run, how many times and from which frame, through the function-entry hook of the lifted code (`fnprof_hit`); read by `tools/analisis/sdk_gaps.py --census` |
