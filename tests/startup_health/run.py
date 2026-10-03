#!/usr/bin/env python3
"""Inject task/bus allocation failures into the production startup and scanner.

Only platform includes and ESP/RTOS boundaries are replaced. The real startup
branch, loop runner, scanner worker, retries and health serializer execute here.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def without_includes(text):
    return "\n".join(line for line in text.splitlines()
                     if not line.startswith(("#include", "#pragma once")))


def function(source, signature):
    begin = source.index(signature)
    brace = source.index("{", begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


def main():
    headers = next((path for path in (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src")
                    if (path / "ArduinoJson.h").is_file()), None)
    if headers is None:
        raise SystemExit("Build a firmware target first to install ArduinoJson.")
    main_source = (ROOT / "src/main.cpp").read_text()
    http_source = (ROOT / "src/http_server.cpp").read_text()
    loop_body = function(main_source, "void brewpiLoop()")
    assert loop_body.index("WaterTest::startBackgroundServices();") < loop_body.index("WaterTest::tick();")
    assert loop_body.index("ow_scanner.retry_if_stopped(oneWirePin);") < loop_body.index("WaterTest::startBackgroundServices();")
    assert "if (ow_scanner.is_running())" in loop_body
    assert "startBackgroundServices" not in function(main_source, "void setup()")
    health_source = main_source[main_source.index("namespace RuntimeHealth {"):main_source.index("void printMem()")]
    production = "\n".join([
        without_includes((ROOT / "src/OneWireSensorPolicy.h").read_text()),
        without_includes((ROOT / "src/OneWireScanner.h").read_text()),
        without_includes((ROOT / "src/OneWireScanner.cpp").read_text()),
        without_includes((ROOT / "src/RuntimeHealth.h").read_text()), health_source,
        function(main_source, "void loop()"),
        function(main_source, "static void runControlLoop(void*)"),
        function(main_source, 'extern "C" void app_main(void)'),
        function(http_source, "namespace {"),
        function(http_source, "void health(JsonDocument &doc)"),
    ])
    with tempfile.TemporaryDirectory(prefix="brewpi-startup-health-") as temp:
        build = Path(temp)
        cpp = build / "test.cpp"
        cpp.write_text((HERE / "NativeEnvironment.h").read_text() + "\n" + production + "\n" +
                       (HERE / "test.inc.cpp").read_text())
        binary = build / "test"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(headers), str(cpp), "-o", str(binary)], check=True)
        for scenario in ("loop_normal", "loop_fallback", "mutex_failure", "bus_failure", "worker_failure",
                         "live_worker", "health_read_only"):
            subprocess.run([str(binary), scenario], check=True)
    print("Startup allocation, scanner retry and read-only health regressions passed.")


if __name__ == "__main__":
    main()
