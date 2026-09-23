# Adaptive pulse-dose glycol cooling

This document describes the original `codex/adaptive-glycol-cooling` port.
On `predictive-glycol-cooling`, pulse-dose is now selectable alongside predictive
coast. See [Selecting the glycol cooling algorithm](GLYCOL_COOLING_SELECTION.md)
for the current setting, switching behavior, and shared diagnostics. The
algorithm described below is unchanged; the historical branch-specific
diagnostic names and firmware revision below do not describe the selector build.

This branch replaces the predictive bang-bang cooling path from
`codex/glycol-heating-v17` (`aafe224`) with the frozen adaptive pulse-dose candidate
benchmarked in the chillsim simulator. It retains the normal BrewPi heating PID,
time-proportional heating windows and capability checks. Cooling needs only the
beer probe, beer setpoint, monotonic time and its own actual pump command history.
It does not use the glycol sensor or glycol temperature.

## Control behavior

`AdaptiveDoseController.cpp` is a portable C++ controller; `GlycolMode.cpp` is the
BrewPi integration. The core observes the unfiltered beer sensor cache at 1 Hz.
Its short temperature mean spans 12 seconds and its slope regression spans 90
seconds. The OneWire conversion worker still determines when physical samples
refresh; reading the cache does not issue an extra bus conversion.

Near the target, the controller budgets a pulse using a learned temperature drop
per pump-second. It waits for the subsequent cooling response, then updates that
gain. Larger errors first use longer probe pulses; a sufficiently large required
dose becomes continuous cooling. Predictive stop conditions still apply during
continuous cooling. There is no mandatory emergency dwell and no pump timer reset
when a diagnostic state changes. The public `GLYCOL_EMERGENCY_COOLING` label means
continuous demand under these same rules, for compatibility with existing state
enumerations.

All cooling calculations are in Celsius and seconds. BrewPi absolute temperature
values have an offset as well as nine fractional bits; the integration decodes
`(value - C_OFFSET) / 512.0`. The display's Fahrenheit/Celsius selection cannot
change the cooling thresholds or gain. The existing cascaded temperature filters
remain available for display and for the retained heating PID.

## Relay timing, interruptions and coexistence with heating

Normal pump ON and OFF intervals are at least two seconds, explicitly defined by
`AdaptiveCooling::Config::min_on_s` and `min_off_s`. This does not reuse the legacy
10-second `GLYCOL_MIN_ON_TIME`, which remains the minimum requested heating duty
slice. A fault or disabled mode immediately stops the pump; safety shutdown can
shorten an ON interval. It must still remain OFF for two seconds before restart.

The existing boot/heat-to-cool direction guard runs before the cooling core.
While heating or blocked by that guard, the core is inhibited and cannot learn a
pump pulse that did not physically run. Interrupted observations are discarded;
the RAM gain and actual prior OFF edge are retained. Existing heating OFF-time
and cool-to-heat direction guards remain in place, using the extended monotonic
clock for elapsed-time checks. Heater operation is excluded from cooling
observations.

Extra calls caused by UI setters do not add extra 1 Hz control samples or PID
integral updates. A valid setpoint change is consumed at the next regular decision
and retains the mechanical relay minimum ON time. Invalid setpoints, sensor faults
and mode changes stop immediately. Mode/setpoint methods apply changed outputs
before returning, and mode changes apply OFF before saving settings to flash.
The 32-bit millisecond counter is extended to 64-bit elapsed time; the integration
works across the old 16-bit seconds wrap and the millisecond counter wrap.

The raw sensor cache is invalid at initialization, on device replacement and
immediately when `TempSensor::update()` gets an invalid reading. A still-connected
sensor with an invalid cache cannot enable cooling. This does not change the
underlying OneWire worker's cached-conversion timeout; stale-but-valid worker data
has the existing worker semantics.

## Settings migration and diagnostic interface

Old `/glycolLearned.json` values (`k`, `C_off`, `L`, `drift_rate`) remain on disk for
rollback. They are **not** interpreted as adaptive gain and are not updated by the
new controller. Legacy `/glycolConfig.json` cooling fields also remain stored but
are ignored by adaptive cooling. Its `trigger_margin` is still used by the retained
heating start logic. The adaptive defaults are explicit, unit-labelled fields in
`AdaptiveCooling::Config`. Gain learning is deliberately RAM-only and returns to
the default after reboot. An explicit new persistence schema would be needed to
change that behavior; this branch does not silently migrate unrelated parameters.

The Telnet `v` control-variable response adds an `adaptiveCooling` object:

- `algorithm`, `phase`, `pumpOn`, `fullCooling`, `coolingOnlyBuild`;
- `rawC`, `sensorValid`, `sensorConnected`, `sensorFailedReads`, `setpointC`,
  `setpointValid`, short mean `temperatureC` and `rateCPerSecond`;
- diagnostic-only `glycolRawC` and `glycolSensorValid`, from the assigned chamber
  sensor cache, with no extra bus read or glycol input to the cooling policy;
- `gainCPerPumpSecond`, `learningUpdates`, `pulseBudgetSeconds`,
  `predictedEndpointC`, `actualOnSeconds`, `lastCompletedOnSeconds`;
- `uptimeMillis`, `coastAgeSeconds`, `coolerActive`, `heaterActive`, `lightActive`;
- explicit relay minimums, `learningPersistence` and `legacyCoolingSettingsIgnored`.

A null pulse budget represents continuous demand. `actualOnSeconds` is the current
uninterrupted ON duration; `lastCompletedOnSeconds` reports the last completed dose.
The `c` control-constants response adds the complete `adaptiveCoolingConfig`.
The test build exposes both diagnostic objects even before glycol mode is enabled,
so its identity can be checked before configuring or starting the experiment.
Legacy temperature reports continue showing their established filtered values;
use `rawC`/`temperatureC` to inspect what this algorithm actually receives/uses.

The dedicated `BREWPI_CHILLSIM_TEST` build disables heating capability and forces
both heater and light outputs OFF, regardless of old settings. It also removes
the normal test-mode bypass of output application. This is specific to the
requested 70.5°F cooling-only hardware experiment; normal builds retain heating.

## Verification

Run the portable core/parity checks described in the other adaptive test folders.
The integration can additionally be checked without Arduino or a connected device:

```sh
python3 tests/adaptive_dose_integration/run.py
```

This compiles the actual branch core, integration, sensor and filter sources with
deterministic I/O shims. It extracts declarations and default/reset bodies from
this branch, and tests both the normal and cooling-only build. Checks cover raw
cache validity and offset decoding, boot inhibition, four-second initial pulses,
unit invariance/ignored legacy settings, duplicate calls, fault/mode OFF edge
retention, target changes during minimum ON time, counter wraps, heater-to-pump
guards, normal heating preservation and the cooling-only override. The board build
is still required to verify platform integration; these tests do not claim GPIO
or hardware sensor coverage.

## Building and installing the experiment image

Use `esp32_chillsim_test` for the known D32 Pro experiment board. It fixes the
physical GPIO25/GPIO26 relay polarity to active-low, holds GPIO25 OFF, and starts
in OFF mode on every boot. The initial OFF levels are applied at the beginning
of `app_main`, before NVS or WiFi initialization. Software cannot guarantee GPIO
levels during the preceding ROM/reset period.

```sh
pio run -e esp32_chillsim_test
pio run -e esp32_chillsim_test -t upload --upload-port /dev/cu.YOUR_ESP32_PORT
```

The existing standalone calibration firmware has no OTA update facility, so the
first installation requires USB. This is only for flashing; control and data
collection use the WiFi Telnet interface on port 23. Do not erase all flash as a
routine installation step. Normal board targets, such as `esp32_wifi_iic`, retain
their normal heater capability and configured relay polarity.

The optional ignored `src/ChillsimTestCredentials.h` can define the two macros in
`src/ChillsimTestCredentials.example.h` to seed the experiment's WiFi credentials.
Existing esp_wifi_config network settings take precedence. Keep the populated
header and any firmware containing private credentials local. With no saved
hostname file, the test image uses `chillsim.local`.

Firmware revision is `v17-adaptive-dose1`. Confirm `V:...adaptiveCooling.algorithm`
is `adaptive-pulse-dose-v1` and `coolingOnlyBuild` is true before starting this
experiment. Assign the beer sensor, diagnostic glycol sensor, and inverted pump
output while OFF; verify the assignments and valid readings before enabling
beer-constant mode at 70.5°F. The chillsim companion runner documents and performs
these checks in `docs/brewpi-hardware-test.md` in that repository.

The controller runs on the ESP32 independently of its Telnet client. Closing a
client does not disable control; explicitly select OFF to end a run. A host
logger cannot guarantee an end-of-run stop while disconnected. A reboot does
stop this test image and requires an explicit new start.
