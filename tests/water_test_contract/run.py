#!/usr/bin/env python3
"""Compile real firmware serializers and exercise the local portal without network.

Use the portal's pinned Python environment. Hardware setup is stubbed; real
WaterTestCore, ArduinoJson, serializers, sample packing, and manifest construction
come from the firmware checkout under test. The receiver uses an in-memory DB.
"""
import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from controller_identity import controller_identity


def definition(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor] + "\n"


def between(source, first, end):
    start = source.index(first)
    return source[start:source.index(end, start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--portal", type=Path)
    mode.add_argument("--firmware-only", action="store_true",
                      help="Compile and exercise firmware serialization without a portal checkout")
    parser.add_argument("--arduinojson", type=Path)
    args = parser.parse_args()
    paths = [args.arduinojson] if args.arduinojson else list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src"))
    headers = next((p for p in paths if (p / "ArduinoJson.h").is_file()), None)
    if headers is None:
        parser.error("Build a firmware target to install ArduinoJson, or provide --arduinojson PATH")
    source = (ROOT / "src/WaterTest.cpp").read_text()
    recording_constants = "\n".join(
        re.search(r"^constexpr [^\n]*\b" + name + r"\s*=[^\n]+;", source, re.M).group(0)
        for name in ("maxRecords", "sparseSampleUs", "denseSampleUs", "edgeWindowUs", "denseRecordBudget")
    )
    functions = between(source, "struct TestControllerDeleter {", "TestControllerPtr testController;")
    functions += between(source, "struct ObservationCounts {", "bool controllerCheckpointed")
    functions += "".join(definition(source, name) for name in (
        "GlycolCooling::Algorithm algorithmForRun(", "const char *controllerPath(", "JsonObjectConst initialController(",
        "TestControllerPtr makeTestController(", "bool controllerPlan(",
        "void controllerRunMetadata(", "bool loadControllerCheckpoint(", "bool checkpointController(",
        "void finishControllers(", "void recordControllerRun(",
        "std::string romOf(", "void common(", "void sensorManifest(", "void outputManifest(",
        "void recordOutput(", "void recordPhase(", "void recordController(",
        "bool recordControllerObservation(", "void closeControllerObservation(",
        "void recordingMetadata(", "Record sampleRecord(",
    ))
    # Extract within the owning function so unrelated records cannot match.
    manifest = between(definition(source, "void startRun("), "char testId[37];", 'if (!controllerPlanReady)')
    manifest += 'assert(controllerPlanReady);\n'
    finish = between(definition(source, "void finishRun("), "terminal.clear();", "closeJournal();")
    harness = (HERE / "harness.cpp").read_text()
    for marker, code in (("RECORDING_CONSTANTS", recording_constants),
                         ("SOURCE_FUNCTIONS", functions), ("MANIFEST_SOURCE", manifest),
                         ("FINISH_SOURCE", finish)):
        harness = harness.replace(f"// @@{marker}@@", code)
    with tempfile.TemporaryDirectory(prefix="water-test-contract-") as temp:
        build = Path(temp)
        cpp, binary = build / "contract.cpp", build / "contract"
        cpp.write_text(harness)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-DARDUINOJSON_SIZEOF_POINTER=4", "-DARDUINOJSON_POOL_CAPACITY=64",
                        "-I", str(headers), "-I", str(ROOT / "src"),
                        "-DCOOLING_IMPLEMENTATION_ID=" + json.dumps(controller_identity(ROOT)), str(cpp), str(ROOT / "src/GlycolCoolingController.cpp"),
                        str(ROOT / "src/PredictiveCoastController.cpp"), str(ROOT / "src/AdaptiveDoseController.cpp"),
                        "-o", str(binary)], check=True)
        completed = subprocess.run([str(binary)], text=True, capture_output=True)
        if completed.returncode:
            print(completed.stderr, file=sys.stderr)
            completed.check_returncode()
        requests = [json.loads(line) for line in completed.stdout.splitlines()]
        check_snapshots(requests)
        dose = subprocess.run([str(binary), "--pulse-dose"], text=True, capture_output=True)
        if dose.returncode:
            print(dose.stderr, file=sys.stderr)
            dose.check_returncode()
        dose_requests = [json.loads(line) for line in dose.stdout.splitlines()]
        check_snapshots(dose_requests)
        if args.firmware_only:
            assert len(requests) > 2
            manifest, terminal = requests[0]["payload"], requests[-1]["payload"]
            assert manifest["test_id"] == terminal["test_id"]
            assert terminal["outcome"] == "completed"
            assert terminal["final_outputs"] == {"pump_on": False, "heater_on": False}
            count = sum(len(request["payload"]["records"]) for request in requests[1:-1])
            print(f"Firmware serializer completed both selected-first campaign fixtures; "
                  f"predictive-first retained {count} records in {len(requests)-2} batches.")
            print("Portal API and analysis integration were not exercised (--firmware-only).")
        else:
            exercise_portal(args.portal.resolve(), requests, build, binary)
            exercise_portal(args.portal.resolve(), dose_requests, build, binary)
    print("Serializer source SHA256:", hashlib.sha256(source.encode()).hexdigest())


def check_snapshots(requests):
    plan = requests[0]["payload"]["test_program"]
    finish = requests[-1]["payload"]
    assert plan["controller_plan_version"] == 3 and plan["order"] == "selected_first"
    assert "controller" not in plan
    assert len(plan["controllers"]) == len(finish["controllers"]) == 2
    assert {c["selection"] for c in plan["controllers"]} == {"pulse_dose", "predictive_coast"}
    rows = [row for request in requests[1:-1] for row in request["payload"]["records"]]
    acquisition = requests[0]["payload"]["acquisition"]
    assert len(rows) <= acquisition["max_records"]
    assert [row["seq"] for row in rows] == list(range(len(rows)))
    assert finish["final_seq_by_boot"][acquisition["recording_boot_id"]] == len(rows) - 1
    boundaries = [row for row in rows if row["type"] == "controller_run"]
    assert [(r["run"], r["event"]) for r in boundaries] == [(1, "started"), (1, "finished"), (2, "started"), (2, "finished")]
    assert boundaries[2]["event_us"] - boundaries[1]["event_us"] >= 180000000
    tail_plan = plan["controller_final_observation"]
    assert tail_plan == {"phase": "controller_final_observe", "min_duration_s": 90,
                         "max_duration_s": 180, "min_valid_beer_samples": 6,
                         "record_valid_beer_samples": True}
    tail = finish["controller_final_observation"]
    assert tail["run"] == 2 and tail["completed"] is True
    assert boundaries[-1]["event_us"] <= tail["started_us"]
    assert tail["ended_us"] - tail["started_us"] >= 90_000_000
    assert tail["ended_us"] - tail["started_us"] <= 180_000_000
    assert tail["last_valid_read_us"] >= tail["started_us"] + 90_000_000
    assert tail["last_valid_read_us"] <= tail["ended_us"] <= finish["t_us"]
    phases = [row for row in rows if row["type"] == "phase" and row["phase"] == tail_plan["phase"]]
    assert len(phases) == 1 and phases[0]["observation_started_us"] == tail["started_us"]
    observed = [row for row in rows if row["type"] == "sample" and row["sensor_role"] == "beer"
                and row["quality"] == "ok" and tail["started_us"] < row["read_us"] <= tail["ended_us"]]
    assert len(observed) == tail["valid_beer_samples"] >= 6
    assert len({row["read_us"] for row in observed}) == len(observed)
    assert all(row["pump_on"] is False and row["read_us"] <= row["t_us"] for row in observed)
    assert max(row["read_us"] for row in observed) == tail["last_valid_read_us"]
    assert all(row["seq"] > boundaries[-1]["seq"] for row in observed)
    after_run = [row for row in rows if row["seq"] > boundaries[-1]["seq"]]
    assert not any(row["type"] == "controller" for row in after_run)
    assert not any(row["type"] == "output" and row["actuator"] == "pump" and row["applied_on"]
                   for row in after_run)
    for initial, final in zip(plan["controllers"], finish["controllers"], strict=True):
        run = initial["run"]
        assert final["run"] == run
        assert abs(initial["target_drop_c"] - 5 / 9) < 1e-6
        assert abs(initial["minimum_headroom_c"] - (5 / 9 + .25)) < 1e-6
        assert initial["max_duration_s"] == 10800
        assert initial["completion_policy"] == "fixed_duration_v1"
        assert initial["observation_goal"] == 3 and "required_episodes" not in initial
        observations = [row for row in rows if row["type"] == "controller_observation" and row["controller_run"] == run]
        assert not any(row["type"] == "controller_episode" for row in rows)
        assert [row["observation"] for row in observations] == list(range(1, len(observations) + 1))
        assert all(row["event"] == "finished" and row["event_us"] <= row["t_us"] for row in observations)
        selected = initial["selection"]
        assert all(row["algorithm"] == selected and type(row["rate_qualified"]) is bool and row["coast_s"] >= 0
                   for row in observations)
        counts = {reason: sum(row["reason"] == reason for row in observations)
                  for reason in ("rate_condition", "coast_time_limit", "rate_unqualified", "interrupted")}
        assert final["observations"] == {
            "goal": 3, "completed": sum(counts.values()) - counts["interrupted"],
            "rate_qualified": counts["rate_condition"], "time_limited": counts["coast_time_limit"],
            "rate_unqualified": counts["rate_unqualified"], "interrupted": counts["interrupted"],
        }
        assert final["status"] == "completed" and final["reason"] == "duration_complete"
        assert final["run_duration_complete"] is True
        for key in ("controller_completed", "controller_timed_out", "controller_episodes_completed",
                    "controller_episodes_required", "controller_initial_settled", "controller_stable_at_end",
                    "controller_observation_complete"):
            assert key not in final and key not in finish
        assert final["ended_us"] - final["started_us"] == 10_800_000_000
        assert all(final["started_us"] <= row["event_us"] <= final["ended_us"] for row in observations)
        header = ROOT / "src" / ("AdaptiveDoseController.h" if selected == "pulse_dose" else "PredictiveCoastController.h")
        config = re.search(r"struct Config \{(.*?)\n\};", header.read_text(), re.S).group(1)
        fields = dict(re.findall(r"double (\w+) = ([0-9.eE+-]+);", config))
        assert set(initial["configuration"]) == set(fields)
        assert set(initial["configuration_exact"]) == set(fields)
        for field, value in fields.items():
            assert struct.pack("!d", float(initial["configuration_exact"][field])) == struct.pack("!d", float(value))
        assert initial["implementation_id"] == controller_identity(ROOT) == final["implementation_id"]
        assert initial["numeric_encoding"] == final["numeric_encoding"] == "binary64-decimal-v1"
        assert initial["initial_tuning"]["learning_updates"] == 0
        assert final["selection"] == selected and final["initialized"] is True
        assert final["learning_status"] in ("learned", "no_updates")
        assert final["started_us"] > 0 and final["captured_at_us"] >= final["ended_us"] > final["started_us"]
        assert isinstance(final["target_c_exact"], str)
        start, end = [row for row in boundaries if row["run"] == run]
        assert start["target_c_exact"] == end["target_c_exact"] == final["target_c_exact"]
        assert abs(start["start_c"] - float(final["target_c_exact"]) - 5/9) < 1e-12
        for field, value in final["final_tuning_exact"].items():
            assert isinstance(value, str) and field in final["final_tuning"]
    selected = plan["controllers"][0]["selection"]
    installation = requests[0]["payload"]["installation"]
    expected_flow = (("measured_at_fermenter", 2.25, "lpm") if selected == "pulse_dose"
                     else ("pump_rating", 200.5, "us_gph"))
    assert tuple(installation[key] for key in ("glycol_flow_source", "glycol_flow_value", "glycol_flow_unit")) == expected_flow


def exercise_portal(portal, requests, scratch, binary):
    os.environ.update(DJANGO_SETTINGS_MODULE="portal.settings", DJANGO_DEBUG="1", SQLITE_PATH=":memory:",
                      MPLCONFIGDIR=str(scratch / "mpl"))
    sys.path.insert(0, str(portal))
    import django
    django.setup()
    from django.core.management import call_command
    from django.test import Client, override_settings
    from fieldtests.analysis import process_next
    from fieldtests.models import Analysis, Experiment, Record
    call_command("migrate", verbosity=0, interactive=False)
    manifest, terminal = requests[0], requests[-1]
    batches = requests[1:-1]
    document = manifest["payload"]
    url = f'/api/v1/water-tests/{document["test_id"]}'
    client = Client(HTTP_HOST="localhost")
    # Terminal first exercises late batches; reversed order exercises monotonic
    # reconstruction without relying on HTTP arrival order. No network is opened.
    order = [manifest, terminal, *reversed(batches)]
    acknowledgements = []
    with override_settings(SECURE_SSL_REDIRECT=True):
        for request in order:
            payload = request["payload"]
            if request is batches[0]:
                # An existing controller may have sent the pre-streaming key
                # order before losing its ACK. Its identifier stays immutable.
                legacy_keys = ("schema_version", "device_guid", "test_id", "records",
                               "first_seq", "last_seq", "batch_id", "boot_id")
                assert set(payload) == set(legacy_keys)
                payload = {key: payload[key] for key in legacy_keys}
            response = getattr(client, request["method"])(url+request["suffix"], json.dumps(payload), content_type="application/json")
            assert response.status_code == 201, (request["suffix"], response.status_code, response.headers, response.content, url)
            ack = response.json()
            assert ack["test_id"] == document["test_id"] and ack["device_guid"] == document["device_guid"]
            if request["suffix"] != "/finish":
                acknowledgements.append({"request": request, "response": ack})
            if request["suffix"] == "/batches":
                assert ack["batch_id"] == request["payload"]["batch_id"]
                assert ack["accepted_ranges"] == [[request["payload"]["first_seq"], request["payload"]["last_seq"]]]
        # Retry the actual bounded serializer against the previously stored legacy key order.
        retry = client.post(url+"/batches", json.dumps(batches[0]["payload"]), content_type="application/json")
        assert retry.status_code == 200 and retry.json()["status"] == "already_present"
        finish_retry = client.put(url+"/finish", json.dumps(terminal["payload"]), content_type="application/json")
        assert finish_retry.status_code == 200
        acknowledgements.append({"request": terminal, "response": finish_retry.json()})
    subprocess.run([str(binary), "--ack"], input="\n".join(json.dumps(item) for item in acknowledgements), text=True, check=True)
    experiment = Experiment.objects.get(pk=document["test_id"])
    assert experiment.upload_status == "complete"
    assert experiment.manifest == document
    assert experiment.finish == terminal["payload"]
    from fieldtests.exports import scientific_manifest, scientific_finish, scientific_record
    assert scientific_manifest(document)["test_program"]["controller_final_observation"] == document["test_program"]["controller_final_observation"]
    assert scientific_finish(terminal["payload"])["controller_final_observation"] == terminal["payload"]["controller_final_observation"]
    tail_phase = next(row for batch in batches for row in batch["payload"]["records"]
                      if row["type"] == "phase" and row["phase"] == "controller_final_observe")
    assert scientific_record(tail_phase)["observation_started_us"] == tail_phase["observation_started_us"]
    assert experiment.outcome == terminal["payload"]["outcome"]
    assert experiment.batches.count() == len(batches)
    retained = experiment.records.order_by("seq")
    assert list(retained.values_list("payload", flat=True)) == [
        record for batch in batches for record in batch["payload"]["records"]
    ]
    invalid = [r for r in retained.filter(kind="sample").values_list("payload", flat=True)
               if r.get("quality") != "ok"]
    assert len(invalid) == 1 and invalid[0]["sensor_role"] == "glycol"
    assert all(invalid[0][field] is None for field in ("raw_c", "raw_sixteenths_c", "adjusted_c", "decision_c"))
    assert process_next()
    analysis = Analysis.objects.get(experiment=experiment)
    assert analysis.input_snapshot["manifest"] == document
    assert analysis.input_snapshot["finish"] == terminal["payload"]
    assert analysis.status == "ready", (analysis.status, analysis.error, analysis.result)
    assert bytes(analysis.chart_png).startswith(b"\x89PNG")
    assert analysis.result["metrics"]["pulse_count"] >= 4
    assert analysis.result["provenance"]["glycol_forcing"]["invalid_samples"] == 1
    selected = document["test_program"]["controllers"][0]["selection"]
    assert len(analysis.result["campaign"]["controller_runs"]) == 2
    print(f'{selected}: accepted {retained.count()} actual firmware-serialized records in {len(batches)} batches; outcome={experiment.outcome}, analysis={analysis.status}.')
    print('Exact initial/final controller snapshots and all raw records are unchanged in retained data and analysis inputs.')
    print('Actual adaptive campaign core sequence replayed successfully; PNG generated with the campaign replay policy.')
    print('HTTP no-redirect, finish-before-batches, reversed uploads, retry acknowledgements, and measured-bath coverage passed.')


if __name__ == "__main__":
    main()
