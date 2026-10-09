"""Local TCP fixture exercises real persistent connections, never hardware."""
from collections import OrderedDict
from contextlib import contextmanager
import http.server
import json
import socket
import threading
import unittest

import check


class CapacityServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, *, limit=4, lru=True, active=False, upload="idle", tick_age=20,
                 persistent=True):
        self.capacity = limit
        self.lru = lru
        self.test_active = active
        self.upload = upload
        self.tick_age = tick_age
        self.persistent = persistent
        self.connections = OrderedDict()
        self.lock = threading.Lock()
        self.requests = []
        self.evictions = 0
        self.rejections = 0
        super().__init__(("127.0.0.1", 0), Handler)

    @staticmethod
    def close_socket(connection):
        try:
            connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        connection.close()

    def get_request(self):
        connection, address = super().get_request()
        with self.lock:
            if len(self.connections) >= self.capacity:
                if self.lru:
                    oldest, _ = self.connections.popitem(last=False)
                    self.close_socket(oldest)
                    self.evictions += 1
                else:
                    self.rejections += 1
                    self.close_socket(connection)
                    return connection, address
            self.connections[connection] = None
        return connection, address

    def handle_error(self, request, client_address):
        # Closing accepted sockets models the old global descriptor exhaustion.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.server.requests.append((self.command, self.path))
        if self.path == check.HEALTH:
            data = {"control_loop": {"started": True, "last_tick_age_ms": self.server.tick_age}}
        elif self.path == check.HEAP:
            data = {"free": 100000, "max": 90000, "frag": 10}
        elif self.path == check.UPTIME:
            data = {"millis": 50000}
        elif self.path == check.WATER_TEST:
            data = {"active": self.server.test_active, "upload_status": self.server.upload, "phase": "idle"}
        else:
            self.send_error(404)
            return
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if self.headers.get("Connection") == "close" or not self.server.persistent:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def finish(self):
        try:
            super().finish()
        finally:
            with self.server.lock:
                self.server.connections.pop(self.connection, None)

    def log_message(self, *args):
        pass


@contextmanager
def server(**kwargs):
    fixture = CapacityServer(**kwargs)
    worker = threading.Thread(target=fixture.serve_forever, daemon=True)
    worker.start()
    try:
        yield fixture
    finally:
        fixture.shutdown()
        with fixture.lock:
            connections = list(fixture.connections)
            fixture.connections.clear()
        for connection in connections:
            fixture.close_socket(connection)
        fixture.server_close()
        worker.join(timeout=2)


class ConnectionCheckTests(unittest.TestCase):
    def run_fixture(self, fixture, **kwargs):
        return check.run_check("127.0.0.1", port=fixture.server_port, timeout=.3, **kwargs)

    def test_four_client_lru_preserves_fresh_admission(self):
        with server() as fixture:
            result = self.run_fixture(fixture, cycles=2)
            self.assertEqual(result["result"], "passed", result)
            self.assertEqual(result["cycles_completed"], 2)
            self.assertTrue(result["cleanup_healthy"])
            self.assertGreater(fixture.evictions, 0)
            pressure = [event for event in result["events"] if event["label"] == "persistent"]
            self.assertEqual(len(pressure), 16)
            self.assertTrue(all(method == "GET" and path in check.ALLOWED_PATHS
                                for method, path in fixture.requests))

    def test_old_global_exhaustion_fails_even_when_cleanup_recovers(self):
        with server(limit=7, lru=False) as fixture:
            result = self.run_fixture(fixture)
            self.assertEqual(result["result"], "failed", result)
            self.assertGreater(fixture.rejections, 0)
            self.assertTrue(result["cleanup_healthy"], result)
            self.assertNotIn("cycles_completed", result)

    def test_busy_or_unknown_preflight_never_opens_pressure_connections(self):
        for active, upload in ((True, "idle"), (False, "pending"), (False, "uploading"),
                               (False, "error"), (False, "unexpected")):
            with self.subTest(active=active, upload=upload), server(active=active, upload=upload) as fixture:
                result = self.run_fixture(fixture)
                self.assertEqual(result["result"], "refused", result)
                self.assertEqual(fixture.requests, [("GET", check.WATER_TEST)])

    def test_stale_control_loop_fails_before_pressure(self):
        with server(tick_age=6000) as fixture:
            result = self.run_fixture(fixture)
            self.assertEqual(result["result"], "failed", result)
            self.assertIn("last_tick_age_ms=6000", result["failures"][0])
            self.assertEqual(len(fixture.requests), 2)

    def test_nonpersistent_server_cannot_claim_pressure_pass(self):
        with server(persistent=False) as fixture:
            result = self.run_fixture(fixture)
            self.assertEqual(result["result"], "failed", result)
            self.assertIn("persistent-connection pressure was not exercised", result["failures"][0])
            self.assertTrue(result["cleanup_healthy"])


if __name__ == "__main__":
    unittest.main()
