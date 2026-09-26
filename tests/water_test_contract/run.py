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
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


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
        "std::string romOf(", "void common(", "void sensorManifest(", "void outputManifest(",
        "void recordOutput(", "void recordPhase(", "Record sampleRecord(",
    ))
    # Extract within the owning function so unrelated records cannot match.
    manifest = between(definition(source, "void startRun("), "char testId[37];", "if (!allocateReserve()")
    finish = between(definition(source, "void finishRun("), "terminal.clear();", "uint32_t counts[maxBoots]")
    harness = (HERE / "harness.cpp").read_text()
    for marker, code in (("SOURCE_FUNCTIONS", functions), ("MANIFEST_SOURCE", manifest),
                         ("FINISH_SOURCE", finish)):
        harness = harness.replace(f"// @@{marker}@@", code)
    with tempfile.TemporaryDirectory(prefix="water-test-contract-") as temp:
        build = Path(temp)
        cpp, binary = build / "contract.cpp", build / "contract"
        cpp.write_text(harness)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                        "-I", str(headers), "-I", str(ROOT / "src"), str(cpp), "-o", str(binary)], check=True)
        completed = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
        requests = [json.loads(line) for line in completed.stdout.splitlines()]
        if args.firmware_only:
            assert len(requests) > 2
            manifest, terminal = requests[0]["payload"], requests[-1]["payload"]
            assert manifest["test_id"] == terminal["test_id"]
            assert terminal["outcome"] == "completed"
            assert terminal["final_outputs"] == {"pump_on": False, "heater_on": False}
            count = sum(len(request["payload"]["records"]) for request in requests[1:-1])
            print(f"Firmware serializer completed its three-pulse fixture: {count} records in {len(requests)-2} batches.")
            print("Portal API and analysis integration were not exercised (--firmware-only).")
        else:
            exercise_portal(args.portal.resolve(), requests, build, binary)
    print("Serializer source SHA256:", hashlib.sha256(source.encode()).hexdigest())


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
    assert Experiment.objects.get().upload_status == "complete"
    invalid = [r for r in Record.objects.filter(kind="sample").values_list("payload", flat=True)
               if r.get("quality") != "ok"]
    assert len(invalid) == 1 and invalid[0]["sensor_role"] == "glycol"
    assert all(invalid[0][field] is None for field in ("raw_c", "raw_sixteenths_c", "adjusted_c", "decision_c"))
    assert process_next()
    analysis = Analysis.objects.get()
    assert analysis.status == "ready", (analysis.status, analysis.error, analysis.result)
    assert analysis.policy_version == "replay-v2" and bytes(analysis.chart_png).startswith(b"\x89PNG")
    assert analysis.result["metrics"]["pulse_count"] == 3
    assert analysis.result["provenance"]["glycol_forcing"]["invalid_samples"] == 1
    print(f'Accepted {Record.objects.count()} actual firmware-serialized records in {len(batches)} batches, including invalid glycol/null fields.')
    print('Actual three-pulse core sequence replayed successfully; PNG generated with replay-v2 and unchanged Chillsim.')
    print('HTTP no-redirect, finish-before-batches, reversed uploads, retry acknowledgements, and measured-bath coverage passed.')


if __name__ == "__main__":
    main()
