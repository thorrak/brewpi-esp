# Crash dumps

Firmware panics save an ELF-format core dump to a dedicated 256 KiB flash
partition. The dump contains task stacks, registers and the panic reason, with
a CRC32 checksum. A later panic replaces the saved dump. Rebooting or downloading
does not erase it.

The dump writer reserves a separate 2 KiB stack. Whole-heap capture and the
dump writer's diagnostic logs are disabled. Flash is written when a panic occurs.

## Install

Flash the updated partition table together with the firmware. An application-only
update, including OTA, cannot install the new partition layout. The LittleFS
partition remains at `0x330000`, with size `0xD0000`.

Keep `firmware.elf` from the exact build flashed to the controller. PlatformIO
places it in `.pio/build/<environment>/firmware.elf`; subsequent builds can replace
it. The ELF's SHA256 identifies which build belongs to a dump. Also retain the
source revision and any uncommitted source changes used for that build.

## Retrieve over Wi-Fi

After a crash, open `http://<controller>/api/crash-dump/` to see whether a valid
dump is available. When the summary can be read, the response includes
`crashed_task`, `panic_reason`, and `app_elf_sha256`.

Download the dump from `http://<controller>/api/crash-dump/download/`, or run:

```sh
curl --fail --show-error http://192.168.5.132/api/crash-dump/download/ \
  --output brewpi-crash-dump.bin
```

The download returns 404 when the partition is empty, 422 for an invalid or
incomplete dump, and 503 when capture is disabled or the partition cannot be
accessed. An interrupted download can be retried. The download does not change
the saved dump.

## Decode

Install [Espressif's esp-coredump tool](https://github.com/espressif/esp-coredump)
in a Python environment, then decode the file using the matching firmware ELF:

```sh
esp-coredump --chip esp32 info_corefile -t raw \
  -c brewpi-crash-dump.bin -g /path/to/xtensa-esp32-elf-gdb \
  /path/to/matching/firmware.elf
```

For ESP32-S2, use `--chip esp32s2` and its matching GDB executable. PlatformIO
installs GDB under its `packages/tool-xtensa-esp-elf-gdb/bin` directory. The
download is `raw` input to the decoder because it includes the flash image header
and checksum around the embedded ELF data.

The decoded output identifies the crashed task, registers and call stack. A dump
cannot reconstruct a crash that occurred before this firmware and partition table
were installed. Abrupt power loss cannot produce a software crash dump.
