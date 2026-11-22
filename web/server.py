"""Loopback HTTP controls for the native simulator, using two anonymous pipes."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]
STATIC = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}
LOGS = {"sessions.csv", "faults.csv", "transactions.csv"}


class Engine:
    def __init__(self, args):
        command = [str(ROOT / "build/voltos"), "--json", "--chargers", str(args.chargers),
                   "--capacity", str(args.capacity), "--logs", str(Path(args.logs).resolve())]
        if args.speed is not None:
            command.extend(("--speed", str(args.speed)))
        if args.aging_minutes is not None:
            command.extend(("--aging-minutes", str(args.aging_minutes)))
        if args.auto_peak:
            command.append("--auto-peak")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                        start_new_session=True)
        self.lock = threading.Lock()
        ready = self.process.stderr.readline().rstrip("\n")
        if not ready.startswith("Logs: "):
            self.close()
            raise RuntimeError(ready or "The native charging engine did not start.")
        self.logs = Path(ready.removeprefix("Logs: "))
        if args.demo:
            self.exchange("demo")

    def exchange(self, command):
        with self.lock:
            try:
                self.process.stdin.write(command + "\n")
                self.process.stdin.flush()
                return json.loads(self.process.stdout.readline())
            except (OSError, ValueError) as error:
                raise RuntimeError("Native engine connection lost. Restart the dashboard.") from error

    def close(self):
        try:
            self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process.stdout.close()
        self.process.stderr.close()


def native_command(data):
    if not isinstance(data, dict):
        raise ValueError("Expected a JSON object.")
    action = data.get("action")
    if action in ("demo", "stop-demo"):
        return action
    if action in ("fault", "repair"):
        charger = data.get("charger")
        if type(charger) is int:
            return f"{action} {charger}"
    if action == "peak" and data.get("mode") in ("on", "off", "auto"):
        return f"peak {data['mode']}"
    if action == "cap" and type(data.get("capacity")) in (int, float):
        return f"cap {data['capacity']}"
    if action == "settings" and type(data.get("capacity")) in (int, float) and data.get("mode") in ("on", "off", "auto"):
        return f"settings {data['capacity']} {data['mode']}"
    if action == "add":
        plate, soc, service, kwh = (data.get(key) for key in ("plate", "soc", "service", "kwh"))
        if (isinstance(plate, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,17}", plate)
                and type(soc) is int and type(kwh) in (int, float)
                and service in ("normal", "medic", "reserved", "premium", "fleet", "emergency")):
            return f"add {plate} {soc} {service} {kwh}"
    raise ValueError("Check the request fields and try again.")


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def allowed(self):
        host = self.headers.get("Host")
        expected = str(self.server.server_port)
        hosts = {f"127.0.0.1:{expected}", f"localhost:{expected}"}
        if host in hosts and self.headers.get("Origin") in (None, f"http://{host}"):
            return True
        self.reply_json({"ok": False, "error": "Use the local dashboard address."}, 403)
        return False

    def reply(self, body, content_type, status=200, filename=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; img-src 'self' data:; frame-ancestors 'none'")
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        self.wfile.write(body)

    def reply_json(self, body, status=200):
        self.reply(json.dumps(body).encode(), "application/json", status)

    def do_GET(self):
        if not self.allowed():
            return
        if self.path == "/api/status":
            try:
                self.reply_json(self.server.engine.exchange("status"))
            except RuntimeError as error:
                self.reply_json({"ok": False, "error": str(error)}, 503)
        elif self.path in STATIC:
            name, mime = STATIC[self.path]
            self.reply((ROOT / "web" / name).read_bytes(), mime)
        elif self.path.startswith("/logs/") and self.path[6:] in LOGS:
            name = self.path[6:]
            try:
                data = (self.server.engine.logs / name).read_bytes()
            except OSError:
                self.reply_json({"ok": False, "error": "This log could not be read."}, 500)
                return
            self.reply(data, "text/csv; charset=utf-8", filename=name)
        else:
            self.reply_json({"ok": False, "error": "Page not found."}, 404)

    def do_POST(self):
        if not self.allowed():
            return
        if self.path != "/api/action":
            self.reply_json({"ok": False, "error": "Action endpoint not found."}, 404)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if self.headers.get_content_type() != "application/json" or not 1 <= size <= 2048:
                raise ValueError("Send a JSON action of up to 2048 bytes.")
            command = native_command(json.loads(self.rfile.read(size)))
            result = self.server.engine.exchange(command)
            self.reply_json(result, 200 if result["ok"] else 400)
        except (ValueError, UnicodeError) as error:
            self.reply_json({"ok": False, "error": str(error)}, 400)
        except RuntimeError as error:
            self.reply_json({"ok": False, "error": str(error)}, 503)

    def log_message(self, fmt, *args):
        if self.path not in ("/api/status", "/favicon.ico"):
            super().log_message(fmt, *args)


class Server(ThreadingHTTPServer):
    # Finish in-flight requests before closing the engine's pipes.
    daemon_threads = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--chargers", type=int, default=4)
    parser.add_argument("--capacity", type=float, default=48)
    parser.add_argument("--speed", type=float, help="Override the manual simulator clock rate")
    parser.add_argument("--aging-minutes", type=float, help="Simulated demo queue minutes before promotion")
    parser.add_argument("--logs", default=str(ROOT / "logs"))
    parser.add_argument("--auto-peak", action="store_true")
    parser.add_argument("--demo", action="store_true")
    args = parser.parse_args()
    try:
        with Server(("127.0.0.1", args.port), Handler) as server:
            server.engine = Engine(args)
            print(f"VoltOS · http://127.0.0.1:{server.server_port}", flush=True)
            print(f"Logs: {server.engine.logs}", flush=True)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
            finally:
                server.server_close()
                server.engine.close()
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"VoltOS: {error}\n")


if __name__ == "__main__":
    main()
