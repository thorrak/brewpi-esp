# Contribute a glycol Chill Test

The **Chill Test** page in the BrewPi web interface records a water-only
cooling experiment. Predictive and pulse-dose cooling remain available for
normal brewing. The experiment temporarily owns the outputs. Its two final
controller challenges start with fresh estimates and leave normal brewing tuning unchanged.

## Participant flow

1. Enable glycol mode and configure the usual beer DS18B20 probe and local wired
   cooling relay. Use a fermenter filled with water at the usual batch volume and
   run the glycol chiller normally.
2. Open **Contribute a Chill Test** in BrewPi's sidebar, which shows this option
   when glycol mode is enabled. The survey appears when a beer probe and local
   cooling relay are configured; otherwise the page shows setup warnings.
   Enter fermenter model/capacity, water volume, cooling arrangement and
   beer-probe placement. Optionally report the glycol pump's rated flow or flow
   measured at the fermenter, with the entered rate and unit. Leave it unknown
   when neither is available; no flow measurement is required.
3. Select the configured **Glycol Temp** DS18B20 probe to measure the bath, if
   available. Otherwise enter the chiller's setpoint or explicitly mark it unknown.
   No additional probe is required.
4. Confirm the water-only preparation and consent to submission, then start.
   Be sure the pump runs during the first test and glycol is circulating.
5. The device runs and records the sequence without needing the browser open.
   The page shows progress, temperatures and a **Stop Test** button.
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

`adaptive-campaign-v1` with decision rules version 2 is one continuous experiment
with a twelve-hour upper limit. It advances when the measurements are informative;
twelve hours is not a target duration. The heater stays OFF throughout.

1. **Baseline.** With the pump OFF, collect at least three minutes of fresh readings.
   Estimate drift and noise over the most recent three minutes, accounting for
   the probe's temperature steps and uncertainty in the fitted trend. Advance
   when the two halves agree. The nominal upper baseline wait is five minutes;
   inadequate sample coverage cannot qualify it.
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
4. **First controller challenge.** Run the selected production cooling algorithm
   with fresh estimates and a fixed target 1°F (5/9°C) below its starting reading.
   It runs for three hours unless stopped by the user or a safety/recording limit.
   The run requires room for the full drop plus 0.25°C above both temperature
   limits. Three completed controller observations are a data-coverage goal;
   temperature-control quality is evaluated separately. The production algorithm
   controls every pulse and learns normally, without changing normal brewing tuning.
5. **Transition and second challenge.** Save the first run, keep the pump OFF,
   and wait for flat temperature or steady warming in two covered windows and
   their combined trend. The minimum wait is the greatest of 180 seconds, the
   observed cooling tail, twice the observed delay, and the relay minimum OFF
   time. After recovery, run the other algorithm with fresh estimates, its own
   1°F target drop and the same three-hour duration. Unresolved recovery after
   30 minutes ends the campaign as inconclusive (`controller_transition_timeout`).
   Insufficient temperature range skips the second run (`controller_headroom`).
6. **Final pump-off recording.** At the second controller's deadline, stop the
   algorithm and pump and save its result. Record at least 90 seconds after the
   wrapper confirms the applied pump output is OFF, retaining at least six
   distinct fresh valid beer readings, including one at or after 90 seconds.
   This separate `controller_final_observe` phase ends inconclusively after
   180 seconds if coverage is insufficient (`controller_final_observation_timeout`).
   The normal stop, freshness and safety limits continue to apply.

The campaign uses one recording and test ID, with separate calibration,
validation and controller analysis roles. Both controllers share the vessel's
thermal history and the campaign's runtime and pump budgets. Their order and
starting temperatures are recorded so comparisons can account for those
conditions. Controller measurements do not enter the earlier model fit.

### Completion and compatibility

Controller plan version 3 declares `completion_policy: fixed_duration_v1`,
`max_duration_s: 10800` and `observation_goal: 3` for each run. Reaching the
scheduled end sets `run_duration_complete`, status `completed` and reason
`duration_complete`, independently of observation count or temperature quality.
Early termination preserves the measurements and outcome for each started run.

Recovery and upload preserve each saved manifest's plan. Earlier version-3
recordings retain their declared 7,200-second duration; version-1/2 episode
records and completion flags remain readable. New tests emit duration completion
and controller observations instead of episode-based completion flags.

### Calibration response and recovery

Pulse decisions require a measured temperature drop. A warming baseline never
adds cooling credit. When the baseline clearly shows natural cooling, the test
subtracts a conservative estimate of that cooling for up to five minutes after
the pulse starts. Beyond that window it freezes response credit from that pulse;
an expired background estimate cannot establish a new response or a larger gain.
A very delayed response during natural cooling can therefore remain inconclusive.

After an isolated pulse, the test looks for the measured cooling to subside into
flat readings or steady warming, independently of the initial drift estimate.
It requires two sufficiently populated windows of 90–1,800
seconds each (longer for a slower observed response), at least 180 seconds OFF,
at least twice the observed response delay, and at least the recovery time
already observed on earlier pulses. A flat trace without detected
cooling does not count as settled. An uninformative pilot can escalate after
five minutes, provided credible cooling has not started. A detected drop of at least 0.125°C, or at least 0.0625°C
with a continuing downward trend, establishes a response and extends observation.
After recovery, the recent pump-off readings refresh the background estimate
for the next pulse. An active cooling tail never becomes a new baseline.
The completion metadata reports this latest baseline's drift, noise, and trend
uncertainty. Both individual windows and their combined trend must show recovery;
a downward probe step between otherwise flat windows still counts as cooling.
Unsettled state and thermal history remain in the recording. An observation has a
six-hour upper limit within the overall twelve-hour cap. An unresolved response
at that limit ends the test as inconclusive with reason `observation_timeout`;
it does not start another pulse or supply a useful-response estimate. Observation
ends sooner when the response meets the measured completion criteria. These rules describe
measured probe behavior and do not prove that all water, hose, and jacket temperatures have equilibrated. Final
validation also waits at least as long as the settled calibration tail already
observed, preventing a shorter flat interval from hiding a known slow response.

Start with water between 8 and 35°C, at least 2°C warmer than the glycol input when
that input is available. Unknown glycol input remains explicitly unknown. The
relay's configured minimum ON/OFF intervals apply, with an absolute minimum of
two seconds. A single pulse is bounded by 30 minutes and total pump operation by
two hours, including both controller challenges. A continuous controller pump run
also stops at 30 minutes, with the `pulse_time_limit` reason. Unsupported minimum
intervals block preflight.

The test stops at a measured drop of 3 + 5/9°C (6.4°F) from its initial water
reading or a 4°C water reading. The extra 1°F reserves space for the second
controller; diagnostic pulse sizing still uses the original 3°C budget.
As in normal OneWire control, a failed read can use the last good
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
microsecond precision. The **durable recording normally keeps one reading per
probe every ten seconds**,
with up to 2,500 additional readings across both probes at approximately two-second
spacing during the first two minutes and after pump edges. Invalid reads may
also use that additional-record budget. Once that budget is consumed, recording
continues at the normal ten-second cadence. The manifest declares these limits;
the uploaded data does not contain every acquisition attempt. During the final
pump-off recording, every fresh valid beer reading after the physical OFF
boundary is retained regardless of those normal sampling budgets. Duplicate,
invalid, queued pre-OFF, and future-dated readings do not satisfy its completion
requirement; a failed durable write fails the campaign rather than qualifying
an unrecorded observation.

Each retained sample preserves its raw Celsius/sixteenth-degree value, calibration
offset, validity, acquisition time, and conversion duration. Invalid readings
remain invalid rather than becoming cached values. In glycol mode the configured
chamber sensor is labeled **Glycol Temp**; selecting it provides actual bath
measurements. A reported chiller setpoint remains metadata, never a fabricated
sensor sample.

Every applied logical pump/heater transition has its own record, independent of
sample cadence. These commands do not prove relay contact state or glycol flow.
Phase records identify the analysis role and block. Controller records preserve
the run's algorithm, controller phase, target, predicted endpoint, cooling
rate, coast estimate, gain estimate, and applied pump state at most once per
minute plus pump transitions. Device GUID, test UUID, recording boot UUID, and
sequence identify immutable survey, measurements, and events. Monotonic
microseconds drive timing; a startup NTP anchor is recorded when available.

The LittleFS journal has a hard ceiling of 12,500 checksummed 40-byte records.
Preflight requires 549,152 free bytes: a 500,000-byte journal plus a 48 KiB
metadata allowance. Of that allowance, 16 KiB is reserved physically for
finish/recovery metadata. The
sampling budget accommodates twelve hours offline plus event records. Extremely
frequent output events, another component consuming flash, or a write failure
can still end acquisition early; the recorded prefix and declared loss remain
available. The journal shares the filesystem with the web UI and settings.

Manifest and finish documents are checksummed and committed by writing a complete
temporary file followed by rename. Metadata is streamed through bounded buffers
instead of building several complete JSON copies in RAM. The manifest is durable
before the first pump command. Uploads begin only after acquisition ends and its finish details are
saved, so network retries cannot stretch a pulse. Every outcome is submitted,
including no measurable response, failure, and stopping during the baseline.
The page shows **Test submitted** only after the server accepts the complete
immutable records and finish, and its receipt has been saved locally.

The upload task is allocated only while a completed recording needs submission,
after the control loop and OneWire sensor worker have started. Its 12 KiB stack
is released after submission or a failed attempt; failures retry after 30 seconds.
HTTP waits run outside the test-state lock. Preparing a batch or committing its
receipt still takes that lock and can briefly delay a display update.

Uploads use a fixed 3 KiB ESP32 workspace to encode one record at a time, then
stream through a 512-byte HTTP buffer. They do not hold a complete batch JSON
document or serialized body in memory. Saved manifest and finish payloads are
validated before opening HTTP and decoded from flash in 256-byte chunks. A
batch always contains the same 12 records (or the final remainder), even after a
lost acknowledgement; a damaged record rejects the whole batch rather than
changing an existing batch ID's contents. Replies are capped at 8 KiB on the
wire, with at most 2,304 bytes of retained JSON allocation for acknowledgement
fields.
Partial writes, truncated replies and missing acknowledgements retain the
original recording and retry without advancing progress. A failed workspace
allocation reports the requested bytes, free heap and largest available block
at the time of failure. Include an open device page when checking upload memory
pressure on hardware; host tests cover the allocation bounds and retry protocol.

If the device page stops responding or connections reset while a recording is
retained, first close all browser tabs for the device, then retry one request to
`/api/health/` followed by `/api/water-test/`. Exhausted network sockets are one
possible cause; these symptoms alone do not prove the test or control loop has
stopped. Check the portal for an accepted result and preserve any available
serial diagnostics before restarting or flashing. Restart recovery retains
pending data, but an unfinished test becomes interrupted rather than continuing.

The web server limits itself to four client sessions and reserves global socket
capacity for other network services. `/api/health/` reports its client count,
client limit and global socket limit under `network`. This protects ordinary
control and uploads as well as the water-test page. After firmware updates,
validate idle connection pressure using `tests/http_connections/check.py`;
see its README for the bounded, read-only procedure.

If the separate control-loop task cannot be allocated, control runs on the
existing main-task stack. Failed OneWire startup is retried every 30 seconds;
a running sensor worker continues to handle its own bus recovery.

After a restart, recovery scans the existing journal's valid prefix. An unfinished
run becomes `interrupted`, with any torn tail declared lost. Recovery records
that the shutdown time was unobserved; it never invents an OFF transition in the
previous boot's clock. Pending uploads retry the same manifest, batches, and
finish, including records the server may already have accepted. Local records
are removed only after a durable server receipt. Saved metadata remains until
normal control has been explicitly resumed. A new test cannot overwrite pending
data. Damaged metadata or a changed completed recording blocks replacement and
holds outputs OFF, with a visible error; its files remain available for recovery.

If startup was interrupted before a manifest was committed and the journal is
provably empty, no measurements or test pump commands occurred. The setup form
allows an explicit new test while outputs stay held OFF. Startup files are kept
until that request; a failed or cancelled retry keeps the hold. Any nonempty or
unreadable journal, existing manifest, or evidence of finalization continues to
require recovery. Do not erase the filesystem to resolve a startup failure.

## Controller snapshots

The durable manifest's `test_program.controllers` array contains both algorithms
in execution order. Each entry records `run`, `selection`, algorithm version,
all effective configuration fields, and numeric initial tuning. `initialization`
is `fresh_defaults`. The same constructors and relay limits build the manifest
snapshots and the controllers used during the test.

The finish document's `controllers` array preserves each run's final tuning from
its actual controller instance, including learning/response counts, target,
start/end times, outcome and observation counts. A completed run is checkpointed
before the next phase. Sparse diagnostic samples are not used to reconstruct
learned values. Each snapshot's `learning_status` distinguishes:

- `learned`: at least one relevant learning or response update occurred.
- `no_updates`: the controller initialized but retained its initial estimates.
- `not_started`: the run ended before the controller was initialized.
- `unavailable_after_restart`: no final snapshot survived for an initialized run.

A saved snapshot retains its values after restart. A missing snapshot leaves
learned values unknown; recorded run boundaries and observations can still be
recovered from the journal. Initial and final snapshots carry `implementation_id`,
a SHA-256 identity of the portable cooling sources, `tuning_schema_version: 1`
and `numeric_encoding: "binary64-decimal-v1"`.

Readable JSON numbers accompany `configuration_exact`, `initial_tuning_exact`,
and `final_tuning_exact`. Those matching objects store each floating-point value
as a 17-digit decimal string that recovers the original binary64 value; integer
update counts remain in the numeric tuning object. Analysis must use these exact
strings for reproducible comparisons. This avoids the precision loss in ordinary
ArduinoJson numeric output. Manifest and finish uploads send their original
checksummed stored JSON bytes, including after a restart, so parsing and
reserializing metadata cannot change an immutable retry.

The portal's starting-versus-learned comparison simulates each cooling
algorithm over its recorded challenge duration. It applies the controller's
relay limits but excludes the campaign safety-stop wrapper, including the total
temperature-drop and pump-time limits. It cannot certify that a physical replay
would run to completion. Predictions outside the installation's tested operating
range are identified separately.

### Controller observations and final recording

`controller_observation` records preserve each production controller's response
closure, including a COAST exit followed by a new ON in the same control tick.
They contain the run, algorithm, observation ordinal, `event_us`, `coast_s`,
`rate_qualified` and reason:

- `rate_condition`: the controller's rate condition ended a covered observation.
- `coast_time_limit`: the controller's observation time limit was reached.
- `rate_unqualified`: the observation closed without adequate rate coverage.
- `interrupted`: a stop or deadline closed an open response.

Each run's `observations` object stores `goal`, `completed`, `rate_qualified`,
`time_limited`, `rate_unqualified` and `interrupted`. The first three completion
reasons count toward `completed`; interruptions are separate. These events are
journaled before the run checkpoint. They describe controller decisions, not
proof that all physical cooling has ended. Recovery reports only saved evidence.

The manifest's `test_program.controller_final_observation` declares the final
pump-off phase, duration bounds and required durable sample count. The finish's
`controller_final_observation` reports `started_us`, end time, last valid read,
sample count, completion and reason. Its phase event records
`observation_started_us`. Recovery preserves the OFF boundary and recorded
samples but marks an interrupted tail incomplete with an unknown end. Controller
performance metrics stop at the run boundary; the tail remains in the raw data.

## Device API

The optional flow survey uses `glycol_flow_source`: `unknown`, `pump_rating`, or
`measured_at_fermenter`. Either known source requires a positive numeric
`glycol_flow_value` and `glycol_flow_unit`: `us_gph`, `us_gpm`, `lph`, or `lpm`.
Gallons are US gallons (3.785411784 liters). Both the entered value and its L/min
conversion must be positive and finite. An omitted source is accepted for older
clients when rate and unit are absent or null. Explicit `unknown` also requires
absent/null rate and unit; conflicting stale values are rejected.

These fields are retained in `manifest.installation` with the original rate and
unit; omitted legacy flow becomes `unknown` with null rate/unit. A pump rating
and a measurement at the fermenter remain distinct. The survey is descriptive
metadata and does not alter controller settings, test sequencing, or simulator
inputs. It does not establish installed glycol mass flow.

- `GET /api/water-test/`: current progress, preflight, temperatures and upload state.
- `GET /api/health/`: control-loop progress and OneWire startup/read diagnostics.
  `control_loop.last_tick_age_ms` shows time since the loop last began an iteration;
  `task_fallback` identifies use of the existing main-task stack. OneWire
  `running`, `last_init_error`, and allocation counters distinguish a worker that
  could not start from one that is running but has no successful probe readings.
  `last_read_age_ms` is null until a real successful read. The request neither
  starts tasks nor accesses the sensor bus.
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

Run the complete hardware-free suite and the cross-repository contract check:

```sh
python3 tests/run_native.py
/path/to/portal/.venv/bin/python tests/water_test_contract/run.py \
  --portal /path/to/glycol_data_collection
cd ui
npx jest --runInBand tests/components/WaterTest.test.js tests/components/AppPolling.test.js \
  tests/mixins/WaterTest.test.js tests/stores/WaterTestStore.test.js
```

Focused checks are documented in [tests/README.md](../tests/README.md).

The cross-repository contract check exercises actual firmware serialization,
receiver ingestion, retry acknowledgements and the real Chillsim worker without
contacting the deployed service or operating any hardware.
