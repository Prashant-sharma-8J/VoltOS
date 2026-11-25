#include "station.hpp"
#include <csignal>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace std;

namespace {
volatile sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

void help() {
    cout << "status                         Display the current snapshot\n"
              << "watch [seconds]                Refresh the console once a second (default 10)\n"
              << "add PLATE SOC SERVICE KWH      Queue a vehicle\n"
              << "  SERVICE: normal | medic\n"
              << "demo                           Start timed traffic with priority aging\n"
              << "stop-demo                      End demo traffic and restore the manual clock\n"
              << "cap KW                         Change grid capacity (5–500 kW)\n"
              << "peak on|off|auto               Use 60% capacity during peak mode\n"
              << "fault CHARGER                  Simulate a fault and automatic recovery\n"
              << "repair CHARGER                 Restore an offline charger immediately\n"
              << "help                           Show commands\n"
              << "quit                           Stop and flush logs\n";
}

double number(const string& text, double min, double max) {
    size_t parsed;
    const double value = stod(text, &parsed);
    if (parsed != text.size() || !isfinite(value) || value < min || value > max)
        throw invalid_argument("Invalid numeric option: " + text);
    return value;
}
}

int main(int argc, char* argv[]) {
    try {
        Station::Config config;
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            const string arg = argv[i];
            auto value = [&]() -> string {
                if (++i >= argc) throw invalid_argument("Missing value for " + arg);
                return argv[i];
            };
            if (arg == "--chargers") {
                const double count = number(value(), 1, 16);
                if (count != floor(count)) throw invalid_argument("Charger count must be an integer.");
                config.chargers = static_cast<int>(count);
            } else if (arg == "--capacity") config.capacity = number(value(), 5, 500);
            else if (arg == "--speed") config.speed = number(value(), 1, 600);
            else if (arg == "--aging-minutes") config.agingMinutes = number(value(), 1, 240);
            else if (arg == "--logs") config.logRoot = value();
            else if (arg == "--auto-peak") config.autoPeak = true;
            else if (arg == "--json") json = true;
            else if (arg == "--help") {
                cout << "voltos [--chargers 1–16] [--capacity 5–500]"
                             " [--speed 1–600] [--aging-minutes 1–240] [--logs directory] [--auto-peak] [--json]\n\n";
                help();
                return 0;
            } else throw invalid_argument("Unknown option: " + arg);
        }
        // Worker threads inherit the masked termination signals. The main
        // thread handles shutdown and can interrupt a terminal read.
        struct sigaction action{};
        action.sa_handler = interrupt;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        Station station(config);
        // The scheduler remains masked and is the parent of every worker.
        pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
        cerr << "Logs: " << station.logDirectory().string() << '\n';
        const bool interactive = !json && isatty(STDIN_FILENO);
        if (!json) cout << "VoltOS · type help for commands\n";
        string line;
        while (!interrupted) {
            if (interactive) cout << "dispatch> " << flush;
            if (!getline(cin, line) || interrupted) break;
            if (line == "quit") {
                if (json) cout << "{\"ok\":true}" << endl;
                break;
            }
            if (!json && line == "help") help();
            else if (!json && line == "status") cout << station.dashboard();
            else if (!json && (line == "watch" || line.rfind("watch ", 0) == 0)) {
                int seconds = 10;
                istringstream args(line.substr(5));
                string extra;
                if ((line.size() > 5 && (!(args >> seconds) || (args >> extra)))
                    || seconds < 1 || seconds > 180) {
                    cout << "Use watch [1–180].\n";
                    continue;
                }
                for (int i = 0; i < seconds && !interrupted; ++i) {
                    if (interactive) cout << "\033[2J\033[H";
                    cout << station.dashboard() << flush;
                    this_thread::sleep_for(chrono::seconds(1));
                }
            } else if (!line.empty() || json) {
                cout << station.command(line) << endl;
            }
        }
    } catch (const exception& error) {
        cerr << "VoltOS: " << error.what() << '\n';
        return 1;
    }
}
