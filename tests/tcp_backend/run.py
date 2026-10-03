#!/usr/bin/env python3
"""Exercise the production TcpBackend with host sockets and syscall failures."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def production_class(source, name):
    start = source.index(f"class {name} ")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    assert source[end] == ";"
    return source[start:end + 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "src/PiStream.h")
    args = parser.parse_args()
    source = args.source.read_text()
    # Compile the classes verbatim; only their unrelated UART/Arduino/RTOS
    # dependencies are omitted. The socket boundary is supplied by test.cpp.
    classes = "\n".join(production_class(source, name)
                        for name in ("PiStreamBackend", "TcpBackend"))
    with tempfile.TemporaryDirectory(prefix="brewpi-tcp-backend-") as temporary:
        build = Path(temporary)
        (build / "production_tcp_backend.h").write_text(classes)
        binary = build / "test"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I" + str(build), str(HERE / "test.cpp"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
