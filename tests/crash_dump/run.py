#!/usr/bin/env python3
"""Exercise production crash-dump routes with simulated flash and HTTP failures."""

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
    with tempfile.TemporaryDirectory(prefix="brewpi-crash-dump-") as temporary:
        for name, flash, elf, sha256 in [
            ("elf-crc32", 1, 1, 0), ("elf-sha256", 1, 1, 1),
            ("binary", 1, 0, 0), ("disabled", 0, 0, 0),
        ]:
            binary = Path(temporary) / name
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                "-Werror", "-pedantic", "-DENABLE_HTTP_INTERFACE",
                f"-DCONFIG_ESP_COREDUMP_ENABLE_TO_FLASH={flash}",
                f"-DCONFIG_ESP_COREDUMP_DATA_FORMAT_ELF={elf}",
                f"-DCONFIG_ESP_COREDUMP_CHECKSUM_SHA256={sha256}",
                "-I", str(HERE / "shim"), "-I", str(ROOT / "src"), "-I", str(json_headers),
                str(ROOT / "src/CrashDump.cpp"), str(ROOT / "src/HttpJsonResponse.cpp"),
                str(HERE / "test.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
    print("Crash dump route regressions passed.")


if __name__ == "__main__":
    main()
