# Predictive glycol cooling

Predictive coast is now one of two selectable cooling algorithms on this branch
and remains the default for existing installations. See
[Selecting the glycol cooling algorithm](GLYCOL_COOLING_SELECTION.md) for the
saved setting, safe switching, and current firmware revision. Both original
controller cores retain the behavior verified below.

`predictive-glycol-cooling` starts from `codex/adaptive-glycol-cooling` at
`f2f72322e166963199fd8ae257a6746faab941ab` and replaces its active glycol cooling
policy with the frozen `PredictiveCoastController` tested in Chillsim. The
portable C++ implementation is `src/PredictiveCoastController.cpp`; BrewPi uses it
through `GlycolMode.cpp`. The original heating PID and heating windows remain.

Only the beer sensor, beer setpoint, elapsed time, and the controller's pump
history are inputs. No glycol sensor, flow estimate, or simulator state is needed.
All arithmetic uses Celsius and seconds regardless of the selected display unit.
This is the simulator's predictive-coast candidate, distinct from the older
predictive bang-bang algorithm on `codex/glycol-heating-v17`.

## Control decisions

At one-second decision intervals, the controller reads the raw beer sensor cache.
A 12-second mean provides the control temperature `T`; a 90-second least-squares
regression provides the slope `r`. These windows include both endpoints. Physical
OneWire conversions still run on the existing worker's schedule.

The estimated endpoint after cooling continues to propagate is:

```
predicted_endpoint = T + min(r, 0) * coast_seconds
```

The coast estimate starts at 300 seconds. Pumping can start when `T - setpoint`
exceeds 0.04°C and the predicted endpoint is above the target. Near the target
(error below 1°C / 1.8°F), the initial exposure budget is:

```
pulse_seconds = max(2, 0.5 * error / budget_gain)
```

The gain starts at 0.03°C per pump-second. An error of at least 1°C permits
continuous pumping immediately. A calculated near-target budget exceeding
120 seconds also permits continuous pumping. Both cases retain every stopping
condition: predicted endpoint at/below target, mean temperature at/below target,
raw temperature at least 0.04°C below target, or exhaustion of a finite budget.
There is no mandatory emergency duration. The
`GLYCOL_FULL_COOLING` state label indicates continuous demand only.

After every completed ON interval, the pump remains OFF for an observation.
Observation ends after at least 450 seconds when the slope magnitude is no more
than 0.00005°C/s, or at 2,400 seconds regardless of slope. Using the temperature
at the **end of this observation**, the controller updates:

- Coast duration from the post-OFF temperature drop divided by the magnitude of
  the slope at shutoff. This requires a negative shutoff slope and at least
  0.04°C of subsequent drop. The observed duration is bounded to 90–1,800 seconds
  and blended into the estimate with weight 0.3.
- Exposure gain from total drop since pump start divided by actual ON duration,
  for intervals lasting at least the configured ON minimum. The observed gain
  has a floor of 0.00005°C/s and is blended with weight 0.5.

These are empirical response estimates, not separately identified transport or
thermowell delays. Passive warming is not subtracted. The observation can delay a
restart for 7.5–40 minutes; this is intentional parity with the tested candidate.
No parameters were retuned during the firmware port.

## Relay timing and heating

Normal cooling ON and OFF intervals each have a two-second minimum. Disabling
control, an invalid setpoint, or an invalid sensor stops immediately; restart
still respects the actual OFF edge. A valid setpoint change honors the current
minimum ON interval before replanning. Duplicate UI calls do not add controller
samples. The wrapping MCU clock is extended to a monotonic 64-bit elapsed clock.

Existing boot and heat-to-cool guards run before the cooling controller so it
cannot learn a pump interval that never occurred. Heating and mode interruptions
discard the current cooling observation. Coast/gain learning survives those
interruptions, and saved learning is restored after reboot. The normal heating PID,
minimum heating slices, and cool-to-heat guard retain their inherited behavior.

`/glycolConfig.json` stores the heating start `trigger_margin`, which defaults to
0.1 degrees in the selected display unit. Predictive cooling defaults are explicit
fields in `PredictiveCooling::Config`.

Learned coast duration, cooling-response gain and their update counters are saved
to flash after learning changes, once the heating and cooling outputs are OFF. Startup
restores the latest saved tuning; missing, malformed or unsupported-version records
use the initial defaults. Pump state, temperature history, incomplete observations
and elapsed timers start fresh.

## Fermentrack and diagnostics

Normal Fermentrack setup, setpoints, temperature reports, and actuator states
continue to work. Enable glycol mode and select Predictive coast to use this policy. Assign the beer
sensor and cooling relay as usual, including the correct relay polarity.

The `v`/`V:` response and HTTP `GET /api/cv/` expose `glycolCooling` with
`algorithm: "predictive-coast-v1"` when predictive is active. The object includes the common temperature,
sensor validity, pump, relay timing, and actuator fields plus:

- `predictedEndpointC`, `coastSeconds`, and coast `learningUpdates`;
- `budgetGainCPerPumpSecond` and gain `responseUpdates`;
- `pulseBudgetSeconds` (null for continuous demand);
- `actualOnSeconds`, `lastCompletedOnSeconds`, and `coastAgeSeconds`.

The `c`/`C:` response exposes the active algorithm's defaults under `glycolCoolingConfig`.
The optional chamber/glycol probe is diagnostic only.
Clients that consume earlier `adaptiveCooling` or `predictiveCooling` objects
must use the common name and check its algorithm identifier. Fermentrack's normal temperature/control interface is
unchanged; 30-second graph samples still cannot show every short pump pulse.

## Build and verify

Build the appropriate board image:

```sh
pio run -e esp32_wifi_iic
```

Portable and integration checks:

```sh
c++ -std=c++11 -O2 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off \
  -Isrc src/PredictiveCoastController.cpp \
  tests/predictive_coast_core/core_test.cpp -o /tmp/predictive-coast-core-test
/tmp/predictive-coast-core-test
python3 tests/predictive_coast_integration/run.py
```

Independent reference replay is documented in
[`tests/predictive_coast_parity/README.md`](../tests/predictive_coast_parity/README.md).
It compares C++ against the frozen Python candidate on all 21 saved 12-hour
predictive traces, both at the original Celsius inputs and with BrewPi's
Fahrenheit/Q9 setpoint encoding. The frozen Python source SHA-256 is
`001d61e81b6ccdc8268b8dbff01896454876699a602ffbdcb1b8fdeaac7d9988`.
Hardware behavior still needs a run with this firmware; simulator replay and
host integration checks do not measure GPIO edges or real probe timing.

Port validation on 2026-09-22 passed: all 1,814,400 replay decisions agreed
exactly, including numeric diagnostics; 46 new native parity regression tests
and the complete 510-test Chillsim suite passed. The portable C++11 checks,
native integration checks, and ESP32 build passed.
The reference controller and physics model were unchanged.
