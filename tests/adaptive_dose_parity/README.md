# Independent adaptive dose parity checks

`adapter.cpp` exposes the unmodified `src/AdaptiveDoseController.cpp` policy to
Python through a narrow C ABI. It contains no cooling decisions. The comparison
implementation is the already frozen `AdaptiveDoseController` in the companion
`chillsim` repository.

From a chillsim checkout with its development dependencies installed:

```sh
.venv/bin/python examples/verify_adaptive_cpp_port.py \
  --source /path/to/this/brewpi/checkout \
  --traces output/analysis/cooling-alternatives-20260921-v1 \
  --output-dir output/analysis/adaptive-cpp-port-20260921
BREWPI_ADAPTIVE_SOURCE=/path/to/this/brewpi/checkout \
  .venv/bin/pytest tests/test_adaptive_native.py -q
```

The replay verifies all 21 saved 12-hour dose traces, using each saved controller
configuration. In particular, the ten-second-relay scenario retains its original
10-second minimum rather than silently using the two-second default. It checks
pump decisions, states, learning counts, gain, temperature average, slope,
predicted endpoint, pulse budget, and completed pulse duration at every tick.
The Python reference must also reproduce the historical saved outputs exactly.

Two passes separate language behavior from input representation:

1. The original Celsius double inputs.
2. The same sensor readings and firmware Q9 Fahrenheit setpoints delivered to
   both implementations. DS18B20 readings are already Q9-exact. A user target of
   70.5°F encodes as 21.388671875°C, equivalent to 70.499609375°F.

The report records source, compiler, library, adapter, and input hashes, and
copies the tested sources. Native compilation disables floating-point
contraction and fast-math. This is host-level C++ behavior verification, not a
claim of instruction-for-instruction ESP32 equivalence.

Independent regression cases also cover fractional/large monotonic timestamps,
hourly slope rebasing, missed ticks, relay minima, sensor faults, disable and
setpoint recovery, learning retention, and interruptible sustained cooling.
Firmware-specific invalid input policies are tested separately: repeated valid
ticks return the cached output; invalid/backward clocks, invalid configuration,
invalid readings, and sample buffer exhaustion fail OFF. Python throws for
invalid clocks/configuration and uses unbounded sample queues, so those are
explicit platform boundary differences.

The controllers share `src/CoolingMeasurements.h`. Older companion harnesses
only hash the original controller header, source and adapter for their native
cache/provenance. Pass a fresh `--cache` directory to the full replay whenever
that shared header changes, and include it in any archived source snapshot and
hash manifest. The pytest harness already uses fresh per-test directories.
