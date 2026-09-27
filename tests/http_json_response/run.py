#!/usr/bin/env python3
"""Exercise the production JSON sender with bounded memory and HTTP failures."""

import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    json_headers = next((path for path in (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src")
                         if (path / "ArduinoJson.h").is_file()), None)
    if json_headers is None:
        raise SystemExit("Build a firmware target first to install ArduinoJson.")
    with tempfile.TemporaryDirectory(prefix="brewpi-http-json-") as temporary:
        binary = Path(temporary) / "test"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
            "-Werror", "-pedantic", "-DENABLE_HTTP_INTERFACE",
            "-I", str(HERE / "shim"), "-I", str(ROOT / "src"), "-I", str(json_headers),
            str(ROOT / "src/HttpJsonResponse.cpp"), str(HERE / "test.cpp"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("HTTP JSON response regressions passed.")


if __name__ == "__main__":
    main()
