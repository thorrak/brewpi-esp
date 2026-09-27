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
    functions = "".join(definition(source, name) for name in (
        "std::unique_ptr<GlycolCooling::Controller> makeTestController(", "void controllerPlan(",
        "void controllerTerminalMetadata(", "std::string romOf(", "void common(", "void sensorManifest(", "void outputManifest(",
        "void recordOutput(", "void recordPhase(", "void recordController(", "void recordingMetadata(", "Record sampleRecord(",
    ))
    # Extract within the owning function so unrelated records cannot match.
    manifest = between(definition(source, "void startRun("), "char testId[37];", 'journal = fs_open(journalPath, "wb");')
    finish = between(definition(source, "void finishRun("), "terminal.clear();", "closeJournal();")
    harness = (HERE / "harness.cpp").read_text()
    for marker, code in (("SOURCE_FUNCTIONS", functions), ("MANIFEST_SOURCE", manifest),
                         ("FINISH_SOURCE", finish)):
        harness = harness.replace(f"// @@{marker}@@", code)
    with tempfile.TemporaryDirectory(prefix="water-test-contract-") as temp:
        build = Path(temp)
        cpp, binary = build / "contract.cpp", build / "contract"
        cpp.write_text(harness)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-I", str(headers), "-I", str(ROOT / "src"),
                        "-DCOOLING_IMPLEMENTATION_ID=" + json.dumps(controller_identity(ROOT)), str(cpp), str(ROOT / "src/GlycolCoolingController.cpp"),
                        str(ROOT / "src/PredictiveCoastController.cpp"), str(ROOT / "src/AdaptiveDoseController.cpp"),
                        "-o", str(binary)], check=True)
        completed = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
        requests = [json.loads(line) for line in completed.stdout.splitlines()]
        check_snapshots(requests)
        dose = subprocess.run([str(binary), "--pulse-dose"], check=True, text=True, capture_output=True)
        dose_requests = [json.loads(line) for line in dose.stdout.splitlines()]
        check_snapshots(dose_requests)
        if args.firmware_only:
            assert len(requests) > 2
            manifest, terminal = requests[0]["payload"], requests[-1]["payload"]
            assert manifest["test_id"] == terminal["test_id"]
            assert terminal["outcome"] == "completed"
            assert terminal["final_outputs"] == {"pump_on": False, "heater_on": False}
            count = sum(len(request["payload"]["records"]) for request in requests[1:-1])
            print(f"Firmware serializer completed its adaptive campaign fixture: {count} records in {len(requests)-2} batches.")
            print("Portal API and analysis integration were not exercised (--firmware-only).")
        else:
            exercise_portal(args.portal.resolve(), requests, build, binary)
            exercise_portal(args.portal.resolve(), dose_requests, build, binary)
    print("Serializer source SHA256:", hashlib.sha256(source.encode()).hexdigest())


def check_snapshots(requests):
    initial = requests[0]["payload"]["test_program"]["controller"]
    final = requests[-1]["payload"]["controller"]
    selected = initial["selection"]
    installation = requests[0]["payload"]["installation"]
    expected_flow = (("measured_at_fermenter", 2.25, "lpm") if selected == "pulse_dose"
                     else ("pump_rating", 200.5, "us_gph"))
    assert tuple(installation[key] for key in ("glycol_flow_source", "glycol_flow_value", "glycol_flow_unit")) == expected_flow
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
    assert final["started_us"] > 0 and final["captured_at_us"] >= final["started_us"]
    assert isinstance(final["target_c_exact"], str)
    for field, value in final["final_tuning_exact"].items():
        assert isinstance(value, str) and field in final["final_tuning"]


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
            response = getattr(client, request["method"])(url+request["suffix"], json.dumps(request["payload"]), content_type="application/json")
            assert response.status_code == 201, (request["suffix"], response.status_code, response.headers, response.content, url)
            ack = response.json()
            assert ack["test_id"] == document["test_id"] and ack["device_guid"] == document["device_guid"]
            if request["suffix"] != "/finish":
                acknowledgements.append({"request": request, "response": ack})
            if request["suffix"] == "/batches":
                assert ack["batch_id"] == request["payload"]["batch_id"]
                assert ack["accepted_ranges"] == [[request["payload"]["first_seq"], request["payload"]["last_seq"]]]
        # Lost acknowledgement: the exact firmware serialization is retryable.
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
    selected = document["test_program"]["controller"]["selection"]
    print(f'{selected}: accepted {retained.count()} actual firmware-serialized records in {len(batches)} batches; outcome={experiment.outcome}, analysis={analysis.status}.')
    print('Exact initial/final controller snapshots and all raw records are unchanged in retained data and analysis inputs.')
    print('Actual adaptive campaign core sequence replayed successfully; PNG generated with the campaign replay policy.')
    print('HTTP no-redirect, finish-before-batches, reversed uploads, retry acknowledgements, and measured-bath coverage passed.')


if __name__ == "__main__":
    main()
