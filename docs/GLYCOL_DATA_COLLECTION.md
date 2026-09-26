# Contribute a glycol water test

The `glycol-data-collection` branch adds a **Water test** page to the normal
BrewPi web interface. Predictive and pulse-dose cooling remain available for
normal brewing. The experiment temporarily owns the outputs and does not train
either controller.

## Participant flow

1. Enable glycol mode and configure the usual beer DS18B20 probe and wired cooling relay. Use a fermenter
   filled with water at the usual batch volume and run the glycol chiller normally.
2. Open **Water test** in BrewPi's web UI. Enter fermenter model/capacity, water
   volume, cooling arrangement and beer-probe placement.
3. If an existing chamber DS18B20 can be moved into the glycol bath, select that
   option and confirm its placement. Otherwise enter the chiller's setpoint or
   explicitly mark it unknown. No additional probe is required.
4. Confirm the water-only preparation and consent to submission, then start.
   Stay nearby for the first pulse to confirm that the pump works.
5. The device runs and records the sequence without needing the browser open.
   The page shows progress, temperatures and a **Stop test** button.
6. After completion, data uploads automatically over **HTTP** to
   `http://chill.fermentrack.net`. Follow the results link on the page.
   Normal control stays OFF until **Resume saved temperature control** is selected.
   Return a moved chamber probe to its normal location before resuming.

Configuration changes and upstream/manual control are held during the experiment
and until explicit resume. The existing Fermentrack connection does not carry this
experiment's data; its normal polling resumes after control is released. No
background research telemetry is collected during ordinary brewing.

## Versioned test program

`cooling-water-v1` uses a five-minute pump-OFF baseline, three cooling pulses and
20 minutes of pump-OFF observation after each pulse. Typical duration is about
65–67 minutes, with a hard 90-minute ceiling. The heater is always OFF.

Start with water between 8 and 35°C, at least 2°C warmer than the glycol
input when that input is available. Unknown glycol input remains explicitly
unknown in the submission.

The pilot is 10 seconds, extended if the configured minimum ON time requires it.
Later pulses are selected from short, bounded durations according to the preceding
observed response. Each pulse is at most 60 seconds; cumulative pump time is at
most 180 seconds. Ordinary transitions respect at least two seconds ON/OFF and
the selected glycol controller’s effective minimum intervals. Unsupported minimum intervals block preflight.

The test stops at a 3°C drop from its initial water reading or a 4°C water reading.
Beer-probe failures, readings older than ten seconds, recording failures and
unaccounted sample loss terminate the experiment. Protective stops switch OFF
immediately; a normal user Stop may wait for the current minimum ON interval.
These are limits on the measured probe, not guarantees of spatially uniform water.

There is no temperature reset between pulses and no assumption that a 20-minute
observation proves every installation has reached equilibrium. Little or no
observed cooling produces an inconclusive submission, not an indefinitely longer
pump run. Stopped, failed and interrupted tests are retained too.

## Measurements and delivery

The OneWire worker records each fresh conversion attempt at approximately two-second
cadence. Raw Celsius/sixteenth-degree values, calibration offsets, validity and
per-probe acquisition times remain separate. The optional chamber sensor is
relabeled `glycol` only for this test, following explicit bath placement. A reported
setpoint is metadata and never becomes a fabricated sensor sample.

Every actual logical pump/heater command transition has its own record, independent
of temperature sampling. These commands do not prove relay contact state or flow.
Device GUID, test UUID, boot UUID and per-boot sequence identify the immutable
survey, configuration, samples and events. Monotonic microseconds drive timing;
one startup NTP attempt supplies a UTC anchor when available. Internet time is
not required to run the experiment.

A compact checksummed LittleFS journal retains the bounded run during network
outages. Preflight checks free capacity. Uploads run on a separate task after
recording ends, so DNS, HTTP retries and server processing cannot stretch a pulse.
Manifest, batch and finish identities remain stable across retries. The page shows
**Test submitted** only after all records and the terminal declaration are accepted.
An upload pending state is independent of whether the experiment completed.

A reboot holds outputs OFF, preserves the recoverable journal and marks an active
run interrupted. It never resumes a pulse sequence or invents the time when power
was lost. A new run cannot replace a pending submission. The journal occupies the
same LittleFS partition as the web UI and device configuration; replacing that
filesystem image removes those local files, including pending contributions.

## Device API

- `GET /api/water-test/`: current progress, preflight, temperatures and upload state.
- `POST /api/water-test/start/`: consent and the normalized survey; accepted work
  is processed by the local sequencing loop.
- `POST /api/water-test/stop/`: request an ordinary stop.
- `POST /api/water-test/resume/`: restore saved normal control; a moved bath probe
  requires `{"probe_returned": true}`.

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
flashing does not update the web UI. Preserve installed configuration and any
pending contribution when choosing an installation/update method.

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
