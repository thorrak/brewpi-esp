# Portable adaptive-dose core checks

Run from the repository root:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -O2 -ffp-contract=off \
  -Isrc src/AdaptiveDoseController.cpp tests/adaptive_dose_core/core_test.cpp \
  -o /tmp/brewpi-adaptive-dose-core-test
/tmp/brewpi-adaptive-dose-core-test
```

The core is a direct port of `AdaptiveDoseController` and `_CoolingBase` in
chillsim's frozen `src/chillsim/controllers/cooling.py`. It receives raw cached
beer temperature in Celsius at one-second intervals. Its defaults match the
simulator; it has no glycol input and no heap allocation. The core holds learning
in memory and exposes its learned gain and update counter for saving and
restoring. Firmware integration stores those values in flash across reboots;
the portable core performs no storage I/O.

Python reference SHA-256:
`001d61e81b6ccdc8268b8dbff01896454876699a602ffbdcb1b8fdeaac7d9988`.

Firmware integration adds these boundary behaviors:

- A duplicate valid timestamp returns the previous output without observing the
  reading twice. The caller schedules at 1 Hz even when BrewPi evaluates state
  more frequently. It must unwrap its hardware clock before calling the core.
- `inhibit(time)` immediately stops and discards an incomplete response, while
  preserving gain and the actual last OFF edge. Use this for invalid sensors,
  explicit disable, mode changes and external heating/switch interlocks.
- Invalid or regressing time fails OFF. Full `reset()` assumes outputs are
  already OFF and is only appropriate for initialization or reconfiguration.
- Ordinary setpoint changes are processed on the next controller tick and
  respect minimum ON/OFF times when cancelling an active pulse. A duplicate tick
  does not accept an ordinary setpoint change early.
- Temperature/setpoint values outside the DS18B20 range, invalid configurations,
  and sample-buffer overflow fail OFF. Finite windows are supported through
  126 seconds for slope and 30 seconds for the mean at 1 Hz; defaults are 90 and
  12 seconds. These buffer bounds replace Python's unbounded deques.
- A continuous dose has an infinite `pulse_budget_s` and `full_cooling = true`.
  It retains every normal stop condition and has no separate emergency dwell.

These checks cover pulse duration, physical relay-edge timing, duplicate and
fault ticks, setpoint cancellation, saturation interruption, retained learning,
learned-tuning round trips and validation, clock-wrap-era epochs, regression
rebasing, measurement gaps, and invalid configuration. Independent parity checks
compare full Python scenario traces.
