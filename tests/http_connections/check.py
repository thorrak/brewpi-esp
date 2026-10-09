#!/usr/bin/env python3
"""Explicit, read-only hardware check for HTTP connection admission under load."""
import argparse
import http.client
import json
import math
import sys
import time

CONNECTIONS = 8
MAX_RESPONSE_BYTES = 65536
HEALTH = "/api/health/"
HEAP = "/api/heap/"
UPTIME = "/api/uptime/"
WATER_TEST = "/api/water-test/"
ALLOWED_PATHS = {HEALTH, HEAP, UPTIME, WATER_TEST}


class CheckFailure(Exception):
    pass


class Refused(CheckFailure):
    pass


def reject_nonfinite_json(value):
    raise ValueError(f"Non-finite JSON constant: {value}")


def request(connection, path, *, keep_alive=False):
    """Consume the entire bounded response before retaining its connection."""
    if path not in ALLOWED_PATHS:
        raise ValueError("Only the four documented read-only API paths are permitted")
    began = time.monotonic()
    connection.request("GET", path, headers={
        "Connection": "keep-alive" if keep_alive else "close",
        "Accept": "application/json",
        "User-Agent": "BrewPi-read-only-connection-check/1",
    })
    response = connection.getresponse()
    try:
        body = response.read(MAX_RESPONSE_BYTES + 1)
        if len(body) > MAX_RESPONSE_BYTES:
            raise CheckFailure(f"{path}: response exceeds {MAX_RESPONSE_BYTES} bytes")
        if response.status != 200:
            raise CheckFailure(f"{path}: HTTP {response.status}, expected 200")
        try:
            document = json.loads(body, parse_constant=reject_nonfinite_json)
        except (ValueError, UnicodeDecodeError) as exc:
            raise CheckFailure(f"{path}: response is not JSON") from exc
        if not isinstance(document, dict):
            raise CheckFailure(f"{path}: expected a JSON object")
        return document, {
            "path": path, "http_status": response.status,
            "elapsed_ms": round((time.monotonic() - began) * 1000, 1),
            "persistent_response": not response.will_close,
        }
    finally:
        response.close()


def check_health(document, maximum_age_ms):
    loop = document.get("control_loop", {})
    age = loop.get("last_tick_age_ms") if isinstance(loop, dict) else None
    if not isinstance(loop, dict) or loop.get("started") is not True:
        raise CheckFailure("Control loop has not reported that it started")
    if isinstance(age, bool) or not isinstance(age, (int, float)) or not math.isfinite(age):
        raise CheckFailure("Control loop has no finite last_tick_age_ms")
    if age < 0 or age > maximum_age_ms:
        raise CheckFailure(f"Control loop last_tick_age_ms={age}, limit={maximum_age_ms}")
    return age


def run_check(host, *, port=80, timeout=3.0, cycles=1, max_tick_age_ms=5000,
              skip_water_test_check=False, allow_busy=False):
    report = {
        "host": host, "port": port, "result": "failed", "cycles_requested": cycles,
        "connections_per_cycle": CONNECTIONS, "socket_timeout_s": timeout,
        "max_tick_age_ms": max_tick_age_ms, "allow_busy": allow_busy,
        "water_test_check_skipped": skip_water_test_check,
        "events": [], "failures": [], "observed_tick_ages_ms": [],
    }
    began = time.monotonic()
    owned = []
    pressure_started = False

    def connect():
        return http.client.HTTPConnection(host, port=port, timeout=timeout)

    def fresh(path, label):
        connection = connect()
        try:
            document, event = request(connection, path)
            event["label"] = label
            report["events"].append(event)
            if path == HEALTH:
                age = check_health(document, max_tick_age_ms)
                report["observed_tick_ages_ms"].append(age)
                event["last_tick_age_ms"] = age
            return document
        finally:
            connection.close()

    try:
        if not skip_water_test_check:
            try:
                water_test = fresh(WATER_TEST, "preflight")
            except (CheckFailure, OSError, http.client.HTTPException) as exc:
                raise Refused(f"Cannot verify idle Chill Test/upload state: {exc}. "
                              "Use --skip-water-test-check only after checking it separately.") from exc
            report["water_test_before"] = {
                key: water_test.get(key) for key in ("active", "upload_status", "phase")
            }
            active = water_test.get("active")
            upload = water_test.get("upload_status")
            known = isinstance(active, bool) and upload in {
                "idle", "not_submitted", "submitted", "pending", "uploading", "error"
            }
            if not known:
                raise Refused("Chill Test/upload state is unknown; no pressure connections opened")
            if (active or upload in {"pending", "uploading", "error"}) and not allow_busy:
                raise Refused(f"Chill Test active={active}, upload_status={upload}; "
                              "no pressure connections opened")
        report["health_before"] = fresh(HEALTH, "before")
        report["heap_before"] = fresh(HEAP, "before")
        report["uptime_before"] = fresh(UPTIME, "before")
        pressure_started = True
        for cycle in range(1, cycles + 1):
            for index in range(1, CONNECTIONS + 1):
                connection = connect()
                owned.append(connection)  # Own it before a connect/request can fail.
                path = (HEALTH, HEAP, UPTIME)[(index - 1) % 3]
                document, event = request(connection, path, keep_alive=True)
                event.update(cycle=cycle, connection=index, label="persistent")
                report["events"].append(event)
                if path == HEALTH:
                    age = check_health(document, max_tick_age_ms)
                    event["last_tick_age_ms"] = age
                    report["observed_tick_ages_ms"].append(age)
                if not event["persistent_response"]:
                    raise CheckFailure("Server closed a completed pressure response; "
                                       "persistent-connection pressure was not exercised")
                # Earlier sockets may already have been closed by LRU. Do not
                # reconnect them or require all eight to remain server-side.
                fresh(HEALTH, f"cycle {cycle}, fresh after connection {index}")
            for connection in owned:
                connection.close()
            owned.clear()
            report["cycles_completed"] = cycle
    except Refused as exc:
        report["result"] = "refused"
        report["failures"].append(str(exc))
    except (CheckFailure, OSError, http.client.HTTPException) as exc:
        report["failures"].append(f"{type(exc).__name__}: {exc}")
    finally:
        for connection in owned:
            connection.close()
        if pressure_started:
            # Give the server a bounded chance to process our FINs. A recovery
            # never erases an admission failure during pressure.
            for attempt in range(1, 4):
                try:
                    report["health_after_cleanup"] = fresh(HEALTH, f"cleanup probe {attempt}")
                    report["cleanup_healthy"] = True
                    break
                except (CheckFailure, OSError, http.client.HTTPException) as exc:
                    report.setdefault("cleanup_probe_errors", []).append(str(exc))
                    if attempt < 3:
                        time.sleep(.2)
            if not report.get("cleanup_healthy"):
                report["failures"].append("Fresh health request did not recover after closing owned connections")
            for path, key in ((HEAP, "heap_after"), (UPTIME, "uptime_after")):
                try:
                    report[key] = fresh(path, "after cleanup")
                except (CheckFailure, OSError, http.client.HTTPException) as exc:
                    report["failures"].append(f"{path} after cleanup: {exc}")
    report["elapsed_s"] = round(time.monotonic() - began, 3)
    if report["observed_tick_ages_ms"]:
        report["max_observed_tick_age_ms"] = max(report["observed_tick_ages_ms"])
    if not report["failures"]:
        report["result"] = "passed"
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Explicit device IP or hostname (no URL or path)")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--timeout", type=float, default=3, help="Socket inactivity timeout, 0.1–10 seconds")
    parser.add_argument("--cycles", type=int, choices=range(1, 4), default=1)
    parser.add_argument("--max-tick-age-ms", type=int, default=5000)
    parser.add_argument("--skip-water-test-check", action="store_true",
                        help="Explicitly bypass unavailable Chill Test status; check it separately first")
    parser.add_argument("--allow-busy", action="store_true",
                        help="Explicitly permit pressure while a known test/upload is busy")
    args = parser.parse_args(argv)
    if not args.host or "/" in args.host or "@" in args.host or any(c.isspace() for c in args.host):
        parser.error("host must be an explicit hostname or IP address, without URL/path")
    if not 1 <= args.port <= 65535 or not .1 <= args.timeout <= 10:
        parser.error("port must be 1–65535 and timeout 0.1–10 seconds")
    if not 1 <= args.max_tick_age_ms <= 60000:
        parser.error("max-tick-age-ms must be 1–60000")
    report = run_check(**vars(args))
    print(json.dumps(report, indent=2, allow_nan=False))
    return 0 if report["result"] == "passed" else 2 if report["result"] == "refused" else 1


if __name__ == "__main__":
    sys.exit(main())
