#!/usr/bin/env python3
"""Check the device's fixed serialization arena across all saved record flag variants."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
headers = next((root / '.pio/libdeps').glob('*/ArduinoJson/src'))
with tempfile.TemporaryDirectory(prefix='water-test-upload-arena-') as folder:
    binary = Path(folder) / 'test'
    for capacity in (64, 128):
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-DARDUINOJSON_SIZEOF_POINTER=4', f'-DARDUINOJSON_POOL_CAPACITY={capacity}',
                    '-I' + str(root / 'src'), '-I' + str(headers), str(here / 'test.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
