"""Exercise the real pthread engine, its CSV output, and the optional HTTP bridge."""
import csv
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/voltos"


def eventually(check, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = check()
        if result:
            return result
        time.sleep(0.04)
    raise AssertionError("Timed out waiting for simulator state")


def power_invariants(state):
    chargers = state["chargers"]
    assert abs(sum(c["kw"] for c in chargers) - state["power"]) <= 0.00002, state
    assert state["power"] <= state["limit"] + 0.000001, state
    assert all(0 <= c["kw"] <= c["maxKw"] and
               ((c["kw"] > 0) == (c["state"] == "Charging")) for c in chargers), state
    return state


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def engine_checks(directory):
    with subprocess.Popen([str(BINARY), "--json", "--chargers", "3", "--capacity", "24",
                           "--speed", "240", "--logs", directory],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8") as process:
        def command(line):
            process.stdin.write(line + "\n")
            process.stdin.flush()
            return json.loads(process.stdout.readline())

        def status():
            return power_invariants(command("status"))

        def active():
            return {c["vehicle"]["plate"]: c for c in status()["chargers"] if c["state"] == "Charging"}

        def ok(line):
            result = command(line)
            assert result["ok"], result
            return result

        startup = process.stderr.readline()
        assert startup.startswith("Logs: "), startup
        log = Path(startup.strip().removeprefix("Logs: "))
        try:
            # Admission at 5 kW is limited to one slot even with three healthy chargers.
            ok("cap 5")
            ok("add FIRST 50 normal 40")
            eventually(lambda: active().get("FIRST"))
            for line in ("add NORMAL 60 normal 0.3", "add BOOKED 30 reserved 0.3",
                         "add FLEET 45 fleet 0.3", "add LOW-A 18 normal 0.3",
                         "add LOW-B 20 premium 0.3", "add MEDIC 1 medic 0.3"):
                ok(line)
            queue = status()["queue"]
            assert [v["plate"] for v in queue] == ["MEDIC", "LOW-A", "LOW-B", "NORMAL", "BOOKED", "FLEET"]
            assert [v["priority"] for v in queue] == ["Medic", "Low battery", "Low battery",
                                                       "Normal", "Normal", "Normal"]
            assert all(a["eta"] <= b["eta"] for a, b in zip(queue, queue[1:]))
            assert not command("add FIRST 12 emergency 1")["ok"]
            assert not command("add BAD 100 normal 1")["ok"]
            assert not command("cap 5oops")["ok"]

            ok("settings 30 on")
            now = eventually(lambda: (current if len(current := active()) == 3 else None))
            assert all(c["kw"] == 6 for c in now.values()), now
            assert status()["limit"] == 18
            active_first = eventually(lambda: (c if (c := active().get("FIRST"))
                                               and c["vehicle"]["delivered"] > 0.08 else None))
            progress = active_first["vehicle"]["delivered"]
            slot = active_first["id"]
            ok(f"fault {slot}")
            assert status()["chargers"][slot - 1]["state"] == "Offline"
            # Once short higher-priority requests finish, FIRST resumes on a different charger.
            resumed = eventually(lambda: (c if (c := active().get("FIRST")) and c["id"] != slot else None))
            assert resumed["vehicle"]["delivered"] >= progress
            assert resumed["vehicle"]["interruptions"] == 1
            assert status()["faultCount"] == 1
            eventually(lambda: status()["chargers"][slot - 1]["state"] == "Available", timeout=14)
            eventually(lambda: status()["completed"] >= 6)
            assert len(rows(log / "transactions.csv")) == 6
            for row in rows(log / "transactions.csv"):
                expected = float(row["energy_kwh"]) * 12 + float(row["charging_minutes"]) * 0.50
                assert abs(float(row["total_inr"]) - expected) <= 0.0051, row

            # Fault every slot, then restore one and ensure the semaphore permits exactly one assignment.
            for c in status()["chargers"]:
                ok(f"fault {c['id']}")
            waiting = eventually(lambda: status()["queue"])
            assert waiting[0]["plate"] == "FIRST" and waiting[0]["eta"] is None
            ok("repair 2")
            eventually(lambda: active().get("FIRST", {}).get("id") == 2)
            assert [r["event"] for r in rows(log / "faults.csv")].count("recovered") == 2
            ok("quit")
            assert process.wait(timeout=3) == 0
            aborted = [r for r in rows(log / "sessions.csv") if r["status"] == "aborted"]
            assert len(aborted) == 1 and aborted[0]["vehicle"] == "FIRST"
            assert int(aborted[0]["interruptions"]) >= 2 and float(aborted[0]["energy_kwh"]) >= progress
        finally:
            if process.poll() is None:
                process.stdin.close()
                process.wait(timeout=4)
    print("PASS · native queue order, power cap, failover, auto-repair, billing and shutdown")


def aging_checks(directory):
    with subprocess.Popen([str(BINARY), "--json", "--chargers", "4",
                           "--speed", "120", "--aging-minutes", "1", "--logs", directory],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8") as process:
        def command(line, ok=True):
            print(line, file=process.stdin, flush=True)
            response = json.loads(process.stdout.readline())
            assert response["ok"] == ok, response
            return response

        def status():
            return power_invariants(command("status"))

        try:
            startup = process.stderr.readline()
            assert startup.startswith("Logs: ")
            log = Path(startup.strip().removeprefix("Logs: "))
            command("cap 5")
            command("add BLOCKER 50 normal 0.3")
            eventually(lambda: status()["chargers"][0]["vehicle"])
            command("add MANUAL 50 normal 0.1")
            command("demo", ok=False)

            # A minute at the manual clock takes 0.5 seconds. The waiting
            # request must keep its Normal priority after that time has passed.
            time.sleep(0.75)
            state = status()
            manual = state["queue"][0]
            assert state["speed"] == 120 and not state["demoActive"]
            assert manual["plate"] == "MANUAL" and manual["priority"] == "Normal"
            assert "promotionIn" not in manual and not manual["aged"]
            eventually(lambda: status()["completed"] == 2)

            command("cap 48")
            command("demo")
            state = eventually(lambda: (s if len((s := status())["queue"]) == 2 else None))
            original = [charger["vehicle"]["plate"] for charger in state["chargers"]]
            assert state["demoActive"] and state["speed"] == 25 and state["agingMinutes"] == 1
            assert original == ["CITY-BUS", "DELIVERY-VAN", "SCHOOL-SHUTTLE", "RIDE-SHARE"]
            assert [vehicle["plate"] for vehicle in state["queue"]] == ["AMBULANCE-01", "TAXI-LOW"]
            command("add BLOCKED 50 normal 3", ok=False)

            aged = eventually(lambda: (s if len((s := status())["queue"]) == 6 else None), timeout=25)
            assert [vehicle["plate"] for vehicle in aged["queue"]] == [
                "AMBULANCE-01", "AMBULANCE-02", "TAXI-LOW",
                "COMMUTER-01", "FAMILY-EV", "COURIER-LOW"
            ]
            assert all(vehicle["rank"] == aged["queue"][2]["rank"] for vehicle in aged["queue"][2:])
            assert [vehicle["aged"] for vehicle in aged["queue"]] == [False, False, False, True, True, False]
            assert [charger["vehicle"]["plate"] for charger in aged["chargers"]] == original
            assert sum(event["type"] == "aging" for event in aged["events"]) == 2

            command("stop-demo")
            stopped = eventually(lambda: (s if not (s := status())["demoActive"] else None))
            assert stopped["speed"] == 120 and not stopped["queue"]
            assert stopped["completed"] == 2
            assert all(c["vehicle"] is None for c in stopped["chargers"])
            assert sum(row["status"] == "aborted" for row in rows(log / "sessions.csv")) == 10
            command("add MANUAL-AGAIN 50 normal 1")
            command("quit")
            assert process.wait(timeout=3) == 0
        finally:
            if process.poll() is None:
                process.stdin.close()
                process.wait(timeout=4)
    print("PASS · timed arrivals, medic FIFO, aging, non-preemption and manual restore")


def http_checks(directory):
    with subprocess.Popen(["python3", str(ROOT / "web/server.py"), "--port", "0",
                           "--logs", directory],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
        try:
            startup = process.stdout.readline().strip()
            assert startup.startswith("VoltOS · http://127.0.0.1:"), (
                process.stderr.read() if not startup else startup)
            address = startup.split()[-1]

            def api(action=None):
                if action is None:
                    request = Request(address + "/api/status")
                else:
                    request = Request(address + "/api/action", json.dumps(action).encode(),
                                      {"Content-Type": "application/json"})
                with urlopen(request, timeout=4) as response:
                    return json.load(response)

            with urlopen(address, timeout=4) as page:
                assert b"Station overview" in page.read()
            assert api({"action": "settings", "capacity": 30, "mode": "on"})["ok"]
            settings = api()
            assert settings["limit"] == 18 and settings["speed"] == 25 and settings["agingMinutes"] == 30
            with ThreadPoolExecutor(max_workers=6) as pool:
                actions = ({"action": "add", "plate": f"EV-{i}", "soc": 40,
                            "service": "normal", "kwh": 1} for i in range(12))
                results = list(pool.map(api, actions))
            assert len({r["id"] for r in results if r["ok"]}) == 12
            state = power_invariants(api())
            assert sum(c["vehicle"] is not None for c in state["chargers"]) + len(state["queue"]) == 12
            assert api({"action": "add", "plate": "MEDIC-01", "soc": 2,
                        "service": "medic", "kwh": 1})["ok"]
            assert api({"action": "add", "plate": "MEDIC-FORM", "soc": 23,
                        "service": "emergency", "kwh": 18})["ok"]
            medic_queue = api()["queue"][:2]
            assert [(vehicle["plate"], vehicle["priority"]) for vehicle in medic_queue] == [
                ("MEDIC-01", "Medic"), ("MEDIC-FORM", "Medic")]
            try:
                api({"action": "add", "plate": "EV-1\nfault 2", "soc": 40, "service": "normal", "kwh": 1})
            except HTTPError as error:
                assert error.code == 400
            else:
                raise AssertionError("Malformed input was accepted")
            with urlopen(address + "/logs/transactions.csv", timeout=4) as response:
                assert "attachment" in response.headers["Content-Disposition"]
                assert b"charging_minutes,total_inr" in response.read()
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                assert process.wait(timeout=8) == 0
    print("PASS · HTTP dashboard, concurrent requests over pipes, validation and CSV download")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="voltos-") as directory:
        engine_checks(directory)
        aging_checks(directory)
        http_checks(directory)
