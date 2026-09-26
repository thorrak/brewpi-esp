# Selecting the glycol cooling algorithm

The `predictive-glycol-cooling` branch includes both simulator-tested cooling
controllers in one firmware image. In **Controller Settings**, enable **Glycol
Mode**, choose **Glycol cooling algorithm**, and save:

| Choice | Saved value | Behavior |
| --- | --- | --- |
| Predictive coast | `predictive_coast` | Predicts the endpoint from the cooling trend and a learned coast duration; permits sustained cooling for large errors. |
| Pulse-dose | `pulse_dose` | Learns temperature reduction per pump-second and applies measured doses separated by observation. |

Predictive coast is the default when upgrading an existing configuration without
this setting. The choice is stored with extended settings and survives reboot.
It affects glycol cooling in beer-constant and beer-profile modes. Normal
chamber control and the shared glycol heating PID retain their existing roles.

## Changing the selection during control

Saving a different algorithm preserves the beer setpoint and control mode.
The controller applies the change at an OFF boundary:

- If pumping, it honors the current minimum ON interval before stopping.
- The incoming controller cannot restart the pump until the minimum OFF interval
  has elapsed from the actual shared OFF edge. Normal defaults are two seconds
  for both intervals.
- Incomplete observations and temperature histories are discarded. Each
  algorithm retains its own learned response values; values are never
  transferred between algorithms. Saved learning is restored after reboot.
- Re-saving the same selection does not interrupt an experiment or reset learning.
- Sensor faults, disabled control, and heating interlocks retain their existing
  immediate-stop behavior and cannot bypass the subsequent OFF minimum.

The setting records the requested algorithm immediately. Runtime diagnostics
distinguish the active selection from a pending request while a minimum ON
interval finishes. An algorithm change does not wait for a whole observation
period to complete. The physical system can continue cooling after shutoff;
changing algorithms cannot cancel that thermal response.

Manual test mode owns its outputs until an explicit mode change and does not
require a sensor or setpoint. Entering or leaving test mode switches outputs OFF
before saving settings. Leaving test mode starts conservative cooling OFF,
heating OFF and direction-switch guards: manually issued relay edges are not
observed by the cooling cores. Learned values are retained.

## Saving learned tuning

Both algorithms save their learned tuning separately: predictive coast saves its
coast duration, cooling-response gain and update counters; pulse-dose saves its
cooling-response gain and update counter. Changed learning is saved to flash once
the heating and cooling outputs are OFF. Both sets of tuning are stored together
in `/glycolTuning.json`, with an atomic file replacement so an interrupted save
does not overwrite the previous snapshot. Failed writes retry after 30 seconds
while the outputs are OFF. A reboot restores the most recent saved values.
Missing, malformed or unsupported-version tuning records use the initial defaults.

Pump state, incomplete observations, temperature samples and elapsed timers are
not restored. Normal startup and relay timing guards still apply. A Chill Test
does not train either cooling algorithm.

## API and logging

Read `GET /api/extended/`. The response includes:

```json
{"extendedSettings":{"glycolCoolingAlgorithm":"predictive_coast"}}
```

Update the selection with the existing authenticated/CSRF-aware settings API:

```http
PUT /api/extended/
Content-Type: application/json

{"glycolCoolingAlgorithm":"pulse_dose"}
```

The endpoint supports partial updates; omitted settings are retained. Invalid
algorithm names, numbers, booleans, or null are rejected. Older clients that do
not send this field preserve the saved selection. The legacy extended-settings
command also accepts the same key and string values.

`GET /api/cv/` and the control-variable Telnet response use one `glycolCooling`
object. It reports `selection`, `requestedSelection`, `switchPending`, and the
active controller's `algorithm` identifier (`predictive-coast-v1` or
`adaptive-pulse-dose-v1`), alongside the common temperature, pump, and timing
fields. Algorithm-specific learned values accompany the active controller.
`glycolCoolingConfig` in control constants reports that controller's settings.
Earlier branch-specific `predictiveCooling`/`adaptiveCooling` diagnostic objects
are replaced by this common interface. Standard Fermentrack temperature,
setpoint, and state reports are unchanged.

Unassigned actuator roles report inactive. This avoids the shared dummy actuator
making unassigned heater/light diagnostics appear active when the fan command
follows cooling.

With `ENABLE_GLYCOL_LOGGING`, `/glycol_log.csv` records glycol state and algorithm
changes after the relay commands have been applied. It reports current raw
Celsius temperature, setpoint, rate, completed/active ON duration, the selected
algorithm's learned values, pulse budget and predicted endpoint. Inapplicable
values are `nan`; continuous budgets are `inf`. The logger retains a single
archive and rotates at 30 KB. The first write after an upgrade archives the old
legacy-controller CSV before creating the new column header. If migration fails,
it leaves the old active file unchanged. The existing clear-log action clears
both files. Manual output commands are not logged as automatic cooling cycles.

## Implementation and installation

`GlycolCoolingController` provides a common interface and shared switch timing
around the `PredictiveCoastController` and `AdaptiveDoseController` policies.
Both cores use `GlycolCoolingMeasurements.h` for bounded sample windows, smoothing and
regression. The helper retains the frozen reference's arithmetic order, expiry
rules, capacity limits and hourly rebasing. Policy, parameters and learned values
remain separate. Only the active core processes temperature samples. `GlycolMode` retains heating
and heating/cooling coordination; `TempControl` retains hardware output and
persistence responsibilities. `ChamberMode` and `ControlContext` are unchanged.

Build the appropriate board target.

The new selector also requires the updated on-device web UI. Firmware and UI
filesystem images are separate artifacts: a firmware-only flash does not update
an existing UI filesystem. Use the repository's normal `buildfs`/`uploadfs`
workflow for the UI, or use the API with the new firmware. `uploadfs` replaces
the filesystem, including saved controller/device settings; record those settings
before using it and restore them afterward. Building this change does not modify
or flash a running controller.

Algorithm equations and original frozen-reference verification are documented in
[predictive coast](PREDICTIVE_GLYCOL_COOLING.md) and
[pulse-dose](ADAPTIVE_GLYCOL_COOLING.md).

## Verification

Run the selector's portable C++ tests from the repository root:

```sh
c++ -std=c++11 -O2 -Wall -Wextra -Werror -pedantic \
  -fno-fast-math -ffp-contract=off -Isrc \
  tests/glycol_cooling_selector/core_test.cpp \
  src/GlycolCoolingController.cpp src/AdaptiveDoseController.cpp \
  src/PredictiveCoastController.cpp -o /tmp/glycol-cooling-selector-test
/tmp/glycol-cooling-selector-test
python3 tests/predictive_coast_integration/run.py
python3 tests/cooling_selector_settings/run.py
```

The selector tests compare 88,000 decisions against the original cores and cover
switching, independent learning, faults, repeated requests, and differing relay
minimums. The integration suite retains its historical directory name but now
exercises both selections with real BrewPi temperature conversion, clock wrap,
heating interlocks, and ArduinoJson diagnostics, with optional logging both enabled
and disabled. It also checks manual
relay ownership with absent sensors, conservative manual-to-automatic protection,
OFF commands before storage access, CSV schema migration, and failed migration.
The settings suite exercises actual persistence, HTTP, and Telnet methods.
These two Python runners need ArduinoJson from a PlatformIO build, or an explicit
`--arduinojson /path/to/ArduinoJson/src`.

From `ui/`, run `npx jest --runInBand tests/stores/ExtendedSettingsStore.test.js`
and `npm run build`. At this revision, the targeted suite passes all 21 tests;
the full UI suite has three existing failures in unrelated sensor/upstream/control
store tests, reproduced on the parent revision.
