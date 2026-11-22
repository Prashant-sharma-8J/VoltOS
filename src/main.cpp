#include "station.hpp"
#include <csignal>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

void help() {
    std::cout << "status                         Display the current snapshot\n"
              << "watch [seconds]                Refresh the console once a second (default 10)\n"
              << "add PLATE SOC SERVICE KWH      Queue a vehicle\n"
              << "  SERVICE: normal | reserved | premium | fleet | emergency\n"
              << "demo                           Queue nine example vehicles at an empty station\n"
              << "cap KW                         Change grid capacity (5–500 kW)\n"
              << "peak on|off|auto               Use 60% capacity during peak mode\n"
              << "fault CHARGER                  Simulate a fault and automatic recovery\n"
              << "repair CHARGER                 Restore an offline charger immediately\n"
              << "help                           Show commands\n"
              << "quit                           Stop and flush logs\n";
}

double number(const std::string& text, double min, double max) {
    size_t parsed;
    const double value = std::stod(text, &parsed);
    if (parsed != text.size() || !std::isfinite(value) || value < min || value > max)
        throw std::invalid_argument("Invalid numeric option: " + text);
    return value;
}
}

int main(int argc, char* argv[]) {
    try {
        Station::Config config;
        bool json = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) throw std::invalid_argument("Missing value for " + arg);
                return argv[i];
            };
            if (arg == "--chargers") {
                const double count = number(value(), 1, 16);
                if (count != std::floor(count)) throw std::invalid_argument("Charger count must be an integer.");
                config.chargers = static_cast<int>(count);
            } else if (arg == "--capacity") config.capacity = number(value(), 5, 500);
            else if (arg == "--speed") config.speed = number(value(), 1, 600);
            else if (arg == "--logs") config.logRoot = value();
            else if (arg == "--auto-peak") config.autoPeak = true;
            else if (arg == "--json") json = true;
            else if (arg == "--help") {
                std::cout << "voltos [--chargers 1–16] [--capacity 5–500]"
                             " [--speed 1–600] [--logs directory] [--auto-peak] [--json]\n\n";
                help();
                return 0;
            } else throw std::invalid_argument("Unknown option: " + arg);
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
        std::cerr << "Logs: " << station.logDirectory().string() << '\n';
        const bool interactive = !json && isatty(STDIN_FILENO);
        if (!json) std::cout << "VoltOS · type help for commands\n";
        std::string line;
        while (!interrupted) {
            if (interactive) std::cout << "dispatch> " << std::flush;
            if (!std::getline(std::cin, line) || interrupted) break;
            if (line == "quit") {
                if (json) std::cout << "{\"ok\":true}" << std::endl;
                break;
            }
            if (!json && line == "help") help();
            else if (!json && line == "status") std::cout << station.dashboard();
            else if (!json && (line == "watch" || line.rfind("watch ", 0) == 0)) {
                int seconds = 10;
                std::istringstream args(line.substr(5));
                std::string extra;
                if ((line.size() > 5 && (!(args >> seconds) || (args >> extra)))
                    || seconds < 1 || seconds > 180) {
                    std::cout << "Use watch [1–180].\n";
                    continue;
                }
                for (int i = 0; i < seconds && !interrupted; ++i) {
                    if (interactive) std::cout << "\033[2J\033[H";
                    std::cout << station.dashboard() << std::flush;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            } else if (!line.empty() || json) {
                std::cout << station.command(line) << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "VoltOS: " << error.what() << '\n';
        return 1;
    }
}
