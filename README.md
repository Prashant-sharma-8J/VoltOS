# VoltOS

A working **Dynamic Load Balancing System** for the Operating System Lab (24B15CS312). It simulates an EV charging station with limited chargers and a shared power budget. The C++ engine runs both the terminal dashboard and the optional browser dashboard.

## Run on Ubuntu

Install a C++17 compiler, GNU Make, and Python 3.9 or newer. Python is needed for the browser dashboard and integration tests.

```sh
sudo apt install build-essential python3
make
```

### Terminal dashboard

```sh
make console
```

Type `demo`, then `watch 10` to see the station update for ten seconds. Use `status` for a single snapshot.

| Command | What it does |
| --- | --- |
| `add EV-8821 12 normal 8` | Register a car at 12% battery with an 8 kWh request. Low-battery priority is calculated automatically. |
| `add MEDIC-02 42 medic 6` | Register a medic request. Service choices are `normal` and `medic`. |
| `peak on` / `peak off` / `peak auto` | Enable, disable, or schedule peak-hour throttling. |
| `cap 24` | Change the station’s base capacity to 24 kW. |
| `fault 1` | Take charger 1 offline and exercise session recovery. |
| `repair 1` | Return an offline charger to service before its automatic recovery. |
| `stop-demo` | End the demo and restore the manual clock. Unfinished demo requests are recorded as aborted. |
| `help` / `quit` | Show commands or shut down and flush the logs. |

### Browser dashboard

```sh
make web
```

Open **http://127.0.0.1:8000**. Use **Add vehicle** to enter your own requests. When the station is empty, **Load demo traffic** starts a separate batch of nine vehicles at 10× with priority aging. Use **Stop demo** to return to manual traffic before the batch finishes. The **Sessions** view has receipts and CSV downloads. The **OS Concepts** tab explains the primitives behind the simulator.

Each launch creates its own station instance and log directory. The browser gateway binds to loopback and launches the same native engine used by the terminal. It uses Python’s standard library and local HTML, CSS, and JavaScript. There is no package installation or internet connection at runtime.

### Configuration

```sh
make console ARGS="--chargers 3 --capacity 36 --speed 1 --auto-peak"
make web ARGS="--chargers 3 --capacity 36 --speed 1 --port 8080 --demo"
```

| Setting | Default | Range or behavior |
| --- | --- | --- |
| `--chargers` | 4 | 1–16. Each charger is capped at 22 kW. |
| `--capacity` | 48 kW | 5–500 kW. Peak throttling uses 60% of this value. |
| `--speed` | 10 | Manual clock in simulated seconds per real second (1–600). The demo runs at 10×. |
| `--aging-minutes` | 30 | 1–240 simulated minutes of demo queue time before a Normal request joins the low-battery group. |
| `--auto-peak` | Off | Enables throttling from 18:00 through 21:59 in the station computer’s local time. |
| `--logs` | `logs/` | Parent directory for per-run CSV logs. |
| `--port` | 8000 | Browser gateway only. |
| `--demo` | Off | Browser gateway only. Queues example arrivals at startup. |

## Lab demonstration

1. Start at the default settings and load demo traffic. Medic and low-battery requests occupy the earliest slots. Each class uses first-in, first-out order.
2. Open **Power settings** and turn peak mode on. Four active sessions at a 48 kW base capacity go from 12 kW each to 7.2 kW each.
3. Select **Test fault** on an active charger. Observe its power fall to zero, the request rejoin the priority queue with energy intact, and the remaining power get redistributed. Recovery takes 12 real seconds. A recovered request resumes in priority order when a healthy slot has enough power.
4. In a manual run, add a new medic vehicle while all slots are busy. It moves ahead of lower-priority waiting requests. Active sessions continue under non-preemptive scheduling.
5. Watch an earlier Normal request age up and move ahead of a later low-battery arrival. For a quick check, start a separate run with `make web ARGS="--aging-minutes 1 --port 8003"` and keep all chargers occupied. The promotion is also recorded in **Activity**.
6. After completion, open **Sessions**. Confirm the receipt totals and compare the fault and transaction logs with the dashboard. Use `--speed 1` for a real-time manual run.

## How the operating-system concepts appear in the code

| Lab concept | Implementation |
| --- | --- |
| POSIX multithreading | `Station::schedule` runs in one dispatcher pthread. `Station::charge` runs in a separate pthread for each active charger assignment. |
| Counting semaphore | A POSIX `sem_t` counts healthy, idle chargers. The dispatcher reserves a token using `sem_trywait`. Completion or repair returns an available token using `sem_post`. An idle fault removes one token. |
| Mutex and condition variable | A `pthread_mutex_t` serializes changes to the queue, charger array, energy totals, and logs. A `pthread_cond_t` wakes the dispatcher and workers when station state changes. |
| Priority scheduling | `std::priority_queue` orders Medic > Low battery (SOC ≤20%) or aged Normal > Normal. The queue uses original arrival order inside each group. During the demo, the dispatcher promotes Normal requests when their accumulated queue time reaches the threshold. Active sessions continue. |
| Producer–consumer | The console or web gateway produces vehicle arrivals. The dispatcher consumes queued requests as healthy chargers and power become available. |
| Arrays and file handling | A fixed-size `std::vector` holds the charger roster. The engine writes persistent CSV records with standard C `fopen`, `fprintf`, and `fflush`. |
| Dynamic resource management | The budget is divided among healthy active chargers, with a per-charger ceiling of 22 kW. New assignments require at least 3 kW per active slot. A lower cap can throttle existing assignments below 3 kW. Power is integrated at the old rate before a change. |
| Deadlock avoidance | The dispatcher checks power and tries the semaphore in the same mutex-protected transition. It never blocks waiting for a token while holding the mutex. A failed thread start rolls back the charger reservation. |
| Fault tolerance | Injected faults simulate a charger going offline. The worker releases power and requeues the same request, preserving its arrival order, energy, and charging time. An offline slot cannot be dispatched until repaired. |
| Optional IPC | The dashboard starts the native binary as a subprocess. Commands and JSON snapshots travel through two anonymous OS pipes on standard input and output. |

## Measurements and logs

Wait estimates project scheduled completions using the current power cap, healthy charger count, and priority order. The browser shows simulated minutes. Estimates change with promotions, new arrivals, faults, and capacity updates.

The billing tariff is **₹12/kWh + ₹0.50 per simulated charging minute**. Charging minutes accumulate only while energy is flowing. A session bills once on completion, even after reassignment. Turnaround includes waiting. Energy and elapsed charging time remain available across charger faults within a run.

Each run writes three CSVs in its printed log directory. An orderly shutdown records unfinished requests as `aborted` in the session log. Open requests are held in memory for the duration of a run.

| File | Contents |
| --- | --- |
| `sessions.csv` | Completed and aborted requests, energy, simulated charging and elapsed time, interruptions |
| `faults.csv` | Fault detection and recovery, including charger and affected request ID |
| `transactions.csv` | Completed sessions and total cost in INR |

## Files and verification

- `src/station.cpp` implements the scheduler, sessions, power budget, fault recovery, and logs.
- `src/main.cpp` provides the console and the line-based protocol used by the gateway.
- `web/server.py` serves the dashboard and connects it to the engine.
- `web/index.html`, `style.css`, and `app.js` implement the browser dashboard.
- `tests/integration.py` exercises priority, power limits, failover, automatic recovery, billing, shutdown, and concurrent HTTP actions.

Run `make test`. The suite takes about 15 seconds and starts a temporary loopback listener for its web checks. The project targets Ubuntu and uses POSIX named semaphores for macOS compatibility.
