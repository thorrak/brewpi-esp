#!/usr/bin/env python3
"""Exercise the production HTTP streaming transport through a deterministic socket facade."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
headers = next((root / '.pio/libdeps').glob('*/ArduinoJson/src'))
source = (root / 'src/WaterTestTransport.cpp').read_text()
for include in ['ESP_BP_WiFi.h', 'esp_http_client.h', 'esp_err.h', 'esp_timer.h']:
    source = source.replace(f'#include "{include}"', '').replace(f'#include <{include}>', '')
with tempfile.TemporaryDirectory(prefix='water-test-transport-') as folder:
    build = Path(folder)
    cpp, binary = build / 'test.cpp', build / 'test'
    cpp.write_text((here / 'environment.h').read_text() + '\n' + source + '\n' +
                   (here / 'test.cpp').read_text())
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DARDUINOJSON_SIZEOF_POINTER=4', '-DARDUINOJSON_POOL_CAPACITY=64', '-I' + str(root / 'src'), '-I' + str(headers),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
