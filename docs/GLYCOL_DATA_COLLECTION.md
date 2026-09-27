# Contribute a glycol Chill Test

The `glycol-data-collection` branch adds a **Chill Test** page to the normal
BrewPi web interface. Predictive and pulse-dose cooling remain available for
normal brewing. The experiment temporarily owns the outputs. Its final controller
challenge starts with fresh estimates and leaves normal brewing tuning unchanged.

## Participant flow

1. Enable glycol mode and configure the usual beer DS18B20 probe and local wired
   cooling relay. Use a fermenter filled with water at the usual batch volume and
   run the glycol chiller normally.
2. Open **Contribute a Chill Test** in BrewPi's sidebar, which shows this option
   when glycol mode is enabled. The survey appears when a beer probe and local
   cooling relay are configured; otherwise the page shows setup warnings.
   Enter fermenter model/capacity, water volume, cooling arrangement and
   beer-probe placement.
3. Select the configured **Glycol Temp** DS18B20 probe to measure the bath, if
   available. Otherwise enter the chiller's setpoint or explicitly mark it unknown.
   No additional probe is required.
4. Confirm the water-only preparation and consent to submission, then start.
   Be sure the pump runs during the first test and glycol is circulating.
5. The device runs and records the sequence without needing the browser open.
   The page shows progress, temperatures and a **Stop test** button.
6. Every outcome uploads automatically over **HTTP** to
   `http://chill.fermentrack.net`: completed, stopped, inconclusive, failed, or
   interrupted by a restart. Follow the results link on the page.
   Normal control stays OFF until **Resume saved temperature control** is selected.

Closing the browser or losing WiFi does not cancel the test. Restarting the device
ends an active test, preserves its durable recording, and queues the recovered
result for upload. A restart never resumes a test pulse. Normal control remains
OFF until explicit resume unless that resume was already saved before the restart.

Configuration changes and upstream/manual control are held during the experiment
and until explicit resume. The existing Fermentrack connection does not carry this
experiment's data; its normal polling resumes after control is released. No
background research telemetry is collected during ordinary brewing.

## Versioned test program

`adaptive-campaign-v1` is one continuous experiment with a twelve-hour upper
limit. It advances when the measurements are informative; twelve hours is not a
target duration. The heater stays OFF throughout.

1. **Baseline.** With the pump OFF, collect at least 60 seconds of fresh readings.
   Advance when the observed drift and noise are consistent. The nominal upper
   baseline window is five minutes; inadequate sample coverage cannot qualify it.
2. **Calibration pulses.** Start with a ten-second pulse. If cooling is too weak,
   escalate through 30, 90, 270, 810, and 1,800 seconds. A calibration pulse ends
   early once cooling exceeds the larger of 0.20°C and four times the measured
   baseline noise, subject to the relay minimum ON time. Once useful cooling is
   found, a shorter contrast pulse supplies a different input duration. Its
   duration and the validation pulses also reserve temperature range for later
   phases, using the response observed so far; five seconds and the configured
   relay minimum form their lower bound.
3. **Reserved validation pattern.** Use two shorter pulses separated by at least
   30 seconds OFF and the configured relay minimum. This portion is reserved for
   checking the fitted response model. Hose, jacket, and water temperatures retain
   their history across every phase; no phase pretends the installation reset.
4. **Controller challenge.** Where enough measured temperature budget remains,
   run the selected production glycol cooling algorithm with fresh estimates and
   a target 0.25°C below the current reading. This phase ends after an actual pump
   cycle and three minutes of stable readings within 0.08°C of target, after
   allowing at least the cooling tail already observed during calibration. It
   otherwise ends after one hour. A timeout is reported explicitly. The controller used for normal
   brewing keeps its existing learned values.

After an isolated pulse, the test looks for the cooling rate to return near the
measured baseline. It requires two sufficiently populated windows of 90–1,800
seconds each (longer for a slower observed response), at least 180 seconds OFF,
and at least twice the observed response delay. A flat trace without detected
cooling does not count as settled. An uninformative pilot can escalate after
five minutes, provided credible cooling has not started. A detected drop of at least 0.125°C, or at least 0.0625°C
with a continuing downward trend, extends observation. Unsettled state and
thermal history remain in the recording. An observation otherwise has a
six-hour upper limit within the overall twelve-hour cap. It still ends as soon
as the response meets the measured completion criteria. These rules describe
measured probe behavior and do not prove that all water, hose, and jacket temperatures have equilibrated. Final
validation also waits at least as long as the settled calibration tail already
observed, preventing a shorter flat interval from hiding a known slow response.

Start with water between 8 and 35°C, at least 2°C warmer than the glycol input when
that input is available. Unknown glycol input remains explicitly unknown. The
relay's configured minimum ON/OFF intervals apply, with an absolute minimum of
two seconds. A single pulse is bounded by 30 minutes and total pump operation by
two hours, including the controller challenge. A continuous controller pump run
also stops at 30 minutes, with the `pulse_time_limit` reason. Unsupported minimum
intervals block preflight.

The test stops at a 3°C measured drop from its initial water reading or a 4°C
water reading. As in normal OneWire control, a failed read can use the last good
beer reading for up to 30 seconds. More than 30 seconds without a good beer
reading, a recording failure, unaccounted sample loss, or an unexpected output
ends the experiment. A fault or temperature limit switches outputs OFF before
diagnostic records are written. A user Stop may wait for the current minimum ON
interval. These are probe limits, not guarantees of uniform water temperature.

Pulse, freshness, and runtime deadlines are checked between journal writes, and
each control tick consumes a fixed snapshot of the sample queue. Flash writes are
synchronous: an operation already in progress can delay a deadline or processing
of a newly arrived fault. Durations are scheduling limits, not hard real-time
cutoffs. Output records preserve the monotonic time when a command was actually
applied, before storage work.

## Measurements and delivery

The OneWire worker acquires fresh conversion attempts approximately every two
seconds. Every fresh beer reading updates the sequencer. The controller consumes
the most recent reading once per second, matching normal glycol control; fast
main-loop calls still check freshness, faults, and protective output limits.
The same once-per-second limit applies to statistical phase decisions; relay
pulse deadlines and protective stops are checked on every call. Statistical
history keeps 2,048 compact readings at least 1.9 seconds apart, with timestamps
relative to the start of the test. This covers both long settling windows while
keeping memory bounded; durable sample timestamps retain their original
microsecond precision. The
**durable recording normally keeps one reading per probe every ten seconds**,
with up to 2,500 additional readings across both probes at approximately two-second
spacing during the first two minutes and after pump edges. Invalid reads may
also use that additional-record budget. Once that budget is consumed, recording
continues at the normal ten-second cadence. The manifest declares these limits;
the uploaded data does not contain every acquisition attempt.

Each retained sample preserves its raw Celsius/sixteenth-degree value, calibration
offset, validity, acquisition time, and conversion duration. Invalid readings
remain invalid rather than becoming cached values. In glycol mode the configured
chamber sensor is labeled **Glycol Temp**; selecting it provides actual bath
measurements. A reported chiller setpoint remains metadata, never a fabricated
sensor sample.

Every applied logical pump/heater transition has its own record, independent of
sample cadence. These commands do not prove relay contact state or glycol flow.
Phase records identify the analysis role and block. Controller records preserve
the selected algorithm, controller phase, target, predicted endpoint, cooling
rate, coast estimate, gain estimate, and applied pump state at most once per
minute plus pump transitions. Device GUID, test UUID, recording boot UUID, and
sequence identify immutable survey, measurements, and events. Monotonic
microseconds drive timing; a startup NTP anchor is recorded when available.

The LittleFS journal has a hard ceiling of 12,500 checksummed 40-byte records.
Preflight requires 532,768 free bytes for the 500,000-byte journal and metadata
allowance; 16 KiB is reserved physically for finish/recovery metadata. The
sampling budget accommodates twelve hours offline plus event records. Extremely
frequent output events, another component consuming flash, or a write failure
can still end acquisition early; the recorded prefix and declared loss remain
available. The journal shares the filesystem with the web UI and settings.

Manifest and finish documents are checksummed and committed by writing a complete
temporary file followed by rename. The manifest is durable before the first pump
command. Uploads begin only after acquisition ends and its finish details are
saved, so network retries cannot stretch a pulse. Every outcome is submitted,
including no measurable response, failure, and stopping during the baseline.
The page shows **Test submitted** only after the server accepts the complete
immutable records and finish, and its receipt has been saved locally.

After a restart, recovery scans the existing journal's valid prefix. An unfinished
run becomes `interrupted`, with any torn tail declared lost. Recovery records
that the shutdown time was unobserved; it never invents an OFF transition in the
previous boot's clock. Pending uploads retry the same manifest, batches, and
finish, including records the server may already have accepted. Local records
are removed only after a durable server receipt. Saved metadata remains until
normal control has been explicitly resumed. A new test cannot overwrite pending
data. Damaged metadata or a changed completed recording blocks replacement and
holds outputs OFF, with a visible error; its files remain available for recovery.

## Controller snapshots

The durable manifest's `test_program.controller` contains the selected algorithm,
its version, all 19 effective configuration fields, and its numeric initial
tuning. The selection is pinned when the campaign starts. `initialization` is
`fresh_defaults`; normal brewing estimates are neither copied into the challenge
nor changed by it. The same constructor and relay limits build the manifest
preview and the controller used later in the test.

The finish document's `controller` object records final tuning directly from the
actual test-controller instance, including learning/response counts. Sparse
controller diagnostic samples are not used to reconstruct those values. An
initialized controller also records `started_us`, `captured_at_us`, `target_c`,
and `target_c_exact`. The snapshot's `learning_status` distinguishes:

- `learned`: at least one relevant learning or response update occurred.
- `no_updates`: the controller initialized but retained its initial estimates.
- `not_started`: the run ended before the controller was initialized.
- `unavailable_after_restart`: an active run was interrupted before a finish
  snapshot could be saved; final tuning and initialization state are unknown.

A finish snapshot already saved before a restart retains its original values.
Both snapshot objects carry `implementation_id`, a SHA-256 identity of the exact
portable cooling implementation sources, plus `tuning_schema_version: 1` and
`numeric_encoding: "binary64-decimal-v1"`.

Readable JSON numbers accompany `configuration_exact`, `initial_tuning_exact`,
and `final_tuning_exact`. Those matching objects store each floating-point value
as a 17-digit decimal string that recovers the original binary64 value; integer
update counts remain in the numeric tuning object. Analysis must use these exact
strings for reproducible comparisons. This avoids the precision loss in ordinary
ArduinoJson numeric output. Manifest and finish uploads send their original
checksummed stored JSON bytes, including after a restart, so parsing and
reserializing metadata cannot change an immutable retry.

The portal's starting-versus-learned comparison simulates the selected cooling
algorithm over the observed challenge duration. It applies the controller's
relay limits but excludes the campaign safety-stop wrapper, including the total
temperature-drop and pump-time limits. It cannot certify that a physical replay
would run to completion. Predictions outside the installation's tested operating
range are identified separately.

## Device API

- `GET /api/water-test/`: current progress, preflight, temperatures and upload state.
- `POST /api/water-test/start/`: consent and the normalized survey; accepted work
  is processed by the local sequencing loop.
- `POST /api/water-test/stop/`: request an ordinary stop.
- `POST /api/water-test/resume/`: restore saved normal control.

An accepted command returns HTTP 202. Rejected commands include a readable JSON
error. Poll status for the resulting state; an HTTP timeout does not mean a start
or stop failed to reach the device. Other configuration requests return HTTP 409
while experiment ownership is held.

The server contract is in the collection portal's `docs/API.md`. Its water-test
API must be reachable on port 80 without an HTTPS redirect. Uploads require no
credential or enablement setting; participant consent, schema validation, request
limits, and immutable retry checks remain enforced.
GUID-keyed result pages are unlisted and display the setup survey and comparison.

## Build and verify

Build a normal supported environment, including its web filesystem:

```sh
pio run -e esp32_wifi_iic
pio run -e esp32_wifi_iic -t buildfs
```

Use `esp32_wifi_tft` for the TFT build or `esp32_s2_wifi` for the S2.
The filesystem target builds the Vue UI and requires Node/npm. Firmware-only
flashing does not update the web UI. Preserve installed configuration and let any
pending contribution finish uploading before restarting for an update.

This feature supports assigned DS18B20 probes and a directly wired cooling
actuator. Wireless sensor/actuator timing is outside this experiment protocol.
A physical relay/sensor smoke test is still required before a distributed release;
compilation and host tests do not demonstrate that a participant's pump is wired
correctly.

The additional host tests are:

```sh
c++ -std=c++17 -Isrc tests/water_test_core/test.cpp -o /tmp/water-test-core
/tmp/water-test-core
c++ -std=c++17 -Isrc -I.pio/libdeps/esp32_wifi_iic/ArduinoJson/src \
  tests/water_test_protocol/test.cpp -o /tmp/water-test-protocol
/tmp/water-test-protocol
python3 tests/water_test_backend/run.py
/path/to/portal/.venv/bin/python tests/water_test_contract/run.py \
  --portal /path/to/glycol_data_collection
cd ui
npx jest --runInBand tests/mixins/WaterTest.test.js tests/stores/WaterTestStore.test.js
```

The cross-repository contract check exercises actual firmware serialization,
receiver ingestion, retry acknowledgements and the real Chillsim worker without
contacting the deployed service or operating any hardware.
