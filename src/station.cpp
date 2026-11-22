#include "station.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {
constexpr double chargerMax = 22;
constexpr double demoSpeed = 10;
constexpr double minimumStartPower = 3;
constexpr double peakFactor = 0.6;
constexpr double energyRate = 12;
constexpr double minuteRate = 0.5;
constexpr int maxQueue = 150;
constexpr auto repairDelay = std::chrono::seconds(12);
enum Priority { medicPriority, lowBatteryPriority, normalPriority };
constexpr const char* priorities[] = {"Medic", "Low battery", "Normal"};

struct Lock {
    pthread_mutex_t& mutex;
    explicit Lock(pthread_mutex_t& m) : mutex(m) { pthread_mutex_lock(&mutex); }
    ~Lock() { pthread_mutex_unlock(&mutex); }
};

void checkThread(int code, const char* action) {
    if (code) throw std::system_error(code, std::generic_category(), action);
}

bool takeSlot(sem_t* semaphore) {
    while (sem_trywait(semaphore) != 0) {
        if (errno == EAGAIN) return false;
        if (errno != EINTR) std::abort();
    }
    return true;
}

void returnSlot(sem_t* semaphore) {
    if (sem_post(semaphore) != 0) std::abort();
}


template <typename... Args>
void logLine(FILE* file, std::string& error, const char* format, Args... args) {
    if (std::fprintf(file, format, args...) < 0 || std::fflush(file) != 0) {
        error = "Could not write CSV log. Check disk space and permissions.";
    }
}

std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}

std::string errorJson(const std::string& message) {
    return "{\"ok\":false,\"error\":" + quoted(message) + "}";
}

long long epochSeconds() { return std::time(nullptr); }

bool validPlate(const std::string& plate) {
    return !plate.empty() && plate.size() <= 18
        && std::isalnum(static_cast<unsigned char>(plate.front()))
        && std::all_of(plate.begin(), plate.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '-';
        });
}


template <typename T>
bool readNumber(std::istringstream& stream, T& value) {
    std::string token;
    if (!(stream >> token)) return false;
    std::istringstream number(token);
    return (number >> value) && number.peek() == std::char_traits<char>::eof();
}

bool atEnd(std::istringstream& stream) {
    std::string extra;
    return !(stream >> extra);
}
}

bool Station::ByPriority::operator()(const std::shared_ptr<Vehicle>& a,
                                   const std::shared_ptr<Vehicle>& b) const {
    if (a->priority != b->priority) return a->priority > b->priority;
    return a->id > b->id;
}

Station::Station(Config config) : config_(std::move(config)), autoPeak_(config_.autoPeak) {
    std::filesystem::create_directories(config_.logRoot);
    std::string pattern = (config_.logRoot / "run-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "Creating log directory");
    logDirectory_ = pattern;
    auto openLog = [&](const char* name, const char* header) {
        Log file(std::fopen((logDirectory_ / name).c_str(), "w"));
        if (!file) throw std::system_error(errno, std::generic_category(), name);
        logLine(file.get(), logError_, "%s\n", header);
        if (!logError_.empty()) throw std::runtime_error(logError_);
        return file;
    };
    sessions_ = openLog("sessions.csv", "unix_time,request,vehicle,energy_kwh,charging_minutes,elapsed_minutes,interruptions,status");
    faults_ = openLog("faults.csv", "unix_time,charger,event,request");
    transactions_ = openLog("transactions.csv", "unix_time,request,vehicle,energy_kwh,charging_minutes,total_inr");

    for (int i = 1; i <= config_.chargers; ++i) chargers_.push_back({this, i, {}, 0, false, {}});

    // Named POSIX semaphores work on Linux and macOS. Unlink after opening so
    // the kernel reclaims the resource when this process ends.
    std::string name = "/cd-" + std::to_string(getpid());
    slots_ = sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, config_.chargers);
    if (slots_ == SEM_FAILED) throw std::system_error(errno, std::generic_category(), "sem_open");
    if (sem_unlink(name.c_str()) != 0) {
        const int error = errno;
        sem_close(slots_);
        throw std::system_error(error, std::generic_category(), "sem_unlink");
    }
    try {
        checkThread(pthread_mutex_init(&mutex_, nullptr), "pthread_mutex_init");
        try {
            checkThread(pthread_cond_init(&changed_, nullptr), "pthread_cond_init");
            try {
                peak_ = autoPeak_ && isPeakHour();
                event("station", "Station started");
                checkThread(pthread_create(&scheduler_, nullptr, schedulerEntry, this), "scheduler pthread_create");
            } catch (...) {
                pthread_cond_destroy(&changed_);
                throw;
            }
        } catch (...) {
            pthread_mutex_destroy(&mutex_);
            throw;
        }
    } catch (...) {
        sem_close(slots_);
        throw;
    }
}

Station::~Station() {
    {
        Lock lock(mutex_);
        stopping_ = true;
        pthread_cond_broadcast(&changed_);
    }
    pthread_join(scheduler_, nullptr);
    {
        Lock lock(mutex_);
        while (workers_ > 0) pthread_cond_wait(&changed_, &mutex_);
        abortQueued();
    }
    pthread_cond_destroy(&changed_);
    pthread_mutex_destroy(&mutex_);
    sem_close(slots_);
}

void* Station::schedulerEntry(void* pointer) {
    static_cast<Station*>(pointer)->schedule();
    return nullptr;
}

void* Station::workerEntry(void* pointer) {
    auto& charger = *static_cast<Charger*>(pointer);
    charger.station->charge(charger);
    return nullptr;
}

void Station::pause(unsigned milliseconds) {
#ifdef __APPLE__
    timespec duration{static_cast<time_t>(milliseconds / 1000),
                      static_cast<long>((milliseconds % 1000) * 1000000)};
    pthread_cond_timedwait_relative_np(&changed_, &mutex_, &duration);
#else
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += milliseconds * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&changed_, &mutex_, &deadline);
#endif
}

void Station::schedule() {
    Lock lock(mutex_);
    while (!stopping_) {
        const auto now = Clock::now();
        if (autoPeak_ && peak_ != isPeakHour()) {
            updateEnergy(now);
            peak_ = !peak_;
            rebalance();
            event("power", peak_ ? "Scheduled peak throttling enabled" : "Scheduled peak throttling ended");
        }
        for (auto& charger : chargers_) {
            if (charger.faulted && !charger.vehicle && now >= charger.repairAt) repair(charger);
        }
        while (dispatch(Clock::now())) {}
        pause(150);
    }
}

bool Station::dispatch(Time now) {
    promoteWaiting(now);
    if (queue_.empty()) return false;
    // Let interrupted workers requeue before admitting a lower-priority request.
    if (std::any_of(chargers_.begin(), chargers_.end(), [](const Charger& charger) {
        return charger.faulted && charger.vehicle;
    })) return false;
    const auto active = std::count_if(chargers_.begin(), chargers_.end(),
        [](const Charger& charger) { return charger.vehicle && !charger.faulted; });
    if ((active + 1) * minimumStartPower > limit() || !takeSlot(slots_)) return false;
    auto slot = std::find_if(chargers_.begin(), chargers_.end(),
        [](const Charger& charger) { return !charger.vehicle && !charger.faulted; });
    if (slot == chargers_.end()) std::abort();

    updateEnergy(now);
    slot->vehicle = queue_.top();
    slot->vehicle->updated = now;
    pthread_t worker{};
    const int error = pthread_create(&worker, nullptr, workerEntry, &*slot);
    if (error) {
        slot->vehicle.reset();
        returnSlot(slots_);
        event("alert", "Session thread could not start. Request retained.");
        return false;
    }
    if (pthread_detach(worker) != 0) std::abort();
    ++workers_;
    slot->vehicle->queuedTime += now - slot->vehicle->queuedSince;
    queue_.pop();
    rebalance();
    event("dispatch", slot->vehicle->plate + " assigned to charger " + std::to_string(slot->id));
    return true;
}

void Station::updateEnergy(Time now) {
    for (auto& charger : chargers_) updateEnergy(charger, now);
}

void Station::updateEnergy(Charger& charger, Time now) {
    if (!charger.vehicle) return;
    auto& vehicle = *charger.vehicle;
    const double hours = std::chrono::duration<double>(now - vehicle.updated).count() * clockSpeed() / 3600;
    vehicle.updated = now;
    if (charger.kw <= 0) return;
    const double energy = std::min(vehicle.target - vehicle.delivered, charger.kw * hours);
    vehicle.delivered += energy;
    vehicle.chargeHours += energy / charger.kw;
}

void Station::charge(Charger& charger) {
    Lock lock(mutex_);
    const auto vehicle = charger.vehicle;
    while (true) {
        const auto now = Clock::now();
        updateEnergy(charger, now);
        if (stopping_ || demoStopping_) {
            shutdownRecord(*vehicle);
            break;
        }
        if (vehicle->target - vehicle->delivered < 1e-9) {
            complete(*vehicle, now);
            break;
        }
        if (charger.faulted) {
            ++vehicle->interruptions;
            queueVehicle(vehicle, now);
            event("requeue", vehicle->plate + " requeued with its delivered energy");
            break;
        }
        pause(80);
    }
    updateEnergy(Clock::now());
    charger.vehicle.reset();
    charger.kw = 0;
    if (!charger.faulted) returnSlot(slots_);
    --workers_;
    finishDemoIfIdle();
    rebalance();
    pthread_cond_broadcast(&changed_);
}

void Station::rebalance() {
    const auto count = std::count_if(chargers_.begin(), chargers_.end(),
        [](const Charger& charger) { return charger.vehicle && !charger.faulted; });
    const double shared = count ? std::min(chargerMax, limit() / count) : 0;
    for (auto& charger : chargers_) {
        charger.kw = charger.vehicle && !charger.faulted ? shared : 0;
    }
    checkInvariant();
}

void Station::markFault(Charger& charger, Time now) {
    updateEnergy(now);
    if (!charger.vehicle && !takeSlot(slots_)) std::abort();
    charger.faulted = true;
    charger.repairAt = now + repairDelay;
    ++faultCount_;
    rebalance();
    logLine(faults_.get(), logError_, "%lld,%d,detected,%d\n", epochSeconds(), charger.id,
            charger.vehicle ? charger.vehicle->id : 0);
    event("fault", "Charger " + std::to_string(charger.id) + " offline. Recovery in 12 seconds.");
    pthread_cond_broadcast(&changed_);
}

void Station::repair(Charger& charger) {
    charger.faulted = false;
    returnSlot(slots_);
    logLine(faults_.get(), logError_, "%lld,%d,recovered,0\n", epochSeconds(), charger.id);
    event("recovery", "Charger " + std::to_string(charger.id) + " returned to service");
}

void Station::complete(const Vehicle& vehicle, Time now) {
    const auto timestamp = epochSeconds();
    const double minutes = vehicle.chargeHours * 60;
    const double elapsed = std::chrono::duration<double>(now - vehicle.arrived).count() * clockSpeed() / 60;
    const double cost = std::round((vehicle.delivered * energyRate + minutes * minuteRate) * 100) / 100;
    logLine(sessions_.get(), logError_, "%lld,%d,%s,%.6f,%.6f,%.6f,%d,completed\n", timestamp, vehicle.id,
        vehicle.plate.c_str(), vehicle.delivered, minutes, elapsed, vehicle.interruptions);
    logLine(transactions_.get(), logError_, "%lld,%d,%s,%.6f,%.6f,%.2f\n", timestamp, vehicle.id,
        vehicle.plate.c_str(), vehicle.delivered, minutes, cost);
    ++completed_;
    completedEnergy_ += vehicle.delivered;
    revenue_ += cost;
    totalElapsedMinutes_ += elapsed;
    receipts_.push_front({vehicle.id, vehicle.plate, vehicle.delivered, minutes, cost, vehicle.interruptions});
    if (receipts_.size() > 25) receipts_.pop_back();
    event("complete", vehicle.plate + " completed charging");
}

void Station::shutdownRecord(const Vehicle& vehicle) {
    const double elapsed = std::chrono::duration<double>(Clock::now() - vehicle.arrived).count() * clockSpeed() / 60;
    logLine(sessions_.get(), logError_, "%lld,%d,%s,%.6f,%.6f,%.6f,%d,aborted\n", epochSeconds(),
        vehicle.id, vehicle.plate.c_str(), vehicle.delivered, vehicle.chargeHours * 60, elapsed,
        vehicle.interruptions);
}

void Station::event(const std::string& type, const std::string& message) {
    events_.push_front({epochSeconds(), type, message});
    if (events_.size() > 40) events_.pop_back();
}

double Station::limit() const { return config_.capacity * (peak_ ? peakFactor : 1); }

double Station::totalPower() const {
    return std::accumulate(chargers_.begin(), chargers_.end(), 0.0,
        [](double sum, const Charger& charger) { return sum + charger.kw; });
}

void Station::checkInvariant() const {
    assert(totalPower() <= limit() + 1e-8);
    assert(std::all_of(chargers_.begin(), chargers_.end(), [](const Charger& charger) {
        return charger.kw >= 0 && charger.kw <= chargerMax
            && ((!charger.vehicle || charger.faulted) ? charger.kw == 0 : charger.kw > 0);
    }));
}

bool Station::isPeakHour() const {
    const time_t now = std::time(nullptr);
    tm local{};
    if (!localtime_r(&now, &local)) return false;
    return local.tm_hour >= 18 && local.tm_hour < 22;
}

int Station::enqueue(const std::string& plate, int soc, const std::string& service, double target) {
    const bool medic = service == "medic" || service == "emergency";
    const int priority = medic ? medicPriority : soc <= 20 ? lowBatteryPriority : normalPriority;
    const auto now = Clock::now();
    const int id = nextId_++;
    queueVehicle(std::make_shared<Vehicle>(Vehicle{
        id, soc, priority, plate, target, 0, 0, 0, now, now, now
    }), now);
    event("arrival", plate + " queued · " + priorities[priority]);
    return id;
}

void Station::queueVehicle(const std::shared_ptr<Vehicle>& vehicle, Time now) {
    vehicle->queuedSince = now;
    if (demoActive_ && vehicle->priority == normalPriority)
        nextPromotion_ = std::min(nextPromotion_, promotionAt(*vehicle));
    queue_.push(vehicle);
}

Station::Time Station::promotionAt(const Vehicle& vehicle) const {
    using SimMinutes = std::chrono::duration<double, std::ratio<60>>;
    const auto threshold = std::chrono::duration_cast<Clock::duration>(
        SimMinutes(config_.agingMinutes / clockSpeed()));
    return vehicle.queuedSince + threshold - vehicle.queuedTime;
}

void Station::promoteWaiting(Time now) {
    if (!demoActive_ || now < nextPromotion_) return;

    // Update priorities outside the heap, then restore heap order. An admitted
    // request may leave an earlier deadline, so always recompute the next one.
    decltype(queue_) reordered;
    nextPromotion_ = Time::max();
    while (!queue_.empty()) {
        auto vehicle = queue_.top();
        queue_.pop();
        if (vehicle->priority == normalPriority) {
            const auto deadline = promotionAt(*vehicle);
            if (deadline <= now) {
                vehicle->priority = lowBatteryPriority;
                vehicle->aged = true;
                event("aging", vehicle->plate + " promoted to the low-battery group after waiting");
            } else {
                nextPromotion_ = std::min(nextPromotion_, deadline);
            }
        }
        reordered.push(vehicle);
    }
    queue_.swap(reordered);
}

void Station::abortQueued() {
    while (!queue_.empty()) {
        shutdownRecord(*queue_.top());
        queue_.pop();
    }
}

double Station::clockSpeed() const { return demoActive_ ? demoSpeed : config_.speed; }

void Station::finishDemoIfIdle() {
    if (!demoActive_ || workers_ || !queue_.empty()) return;
    demoActive_ = false;
    demoStopping_ = false;
    nextPromotion_ = Time::max();
    event("station", "Demo ended. Manual clock restored.");
}

std::vector<std::shared_ptr<Station::Vehicle>> Station::queued() const {
    auto copy = queue_;
    std::vector<std::shared_ptr<Vehicle>> result;
    while (!copy.empty()) {
        result.push_back(copy.top());
        copy.pop();
    }
    return result;
}

std::vector<double> Station::waitEstimates(const std::vector<std::shared_ptr<Vehicle>>& queue) const {
    const auto healthy = std::count_if(chargers_.begin(), chargers_.end(),
        [](const Charger& charger) { return !charger.faulted; });
    if (!healthy) return std::vector<double>(queue.size(), -1);
    const size_t slots = std::min(static_cast<size_t>(healthy),
                                  static_cast<size_t>(limit() / minimumStartPower));
    if (!slots) return std::vector<double>(queue.size(), -1);

    // Project completion events at the current cap. Each departure changes the
    // remaining sessions' power share. Use the current dispatch order.
    std::vector<double> remaining, estimates;
    for (const auto& charger : chargers_) {
        if (charger.vehicle && !charger.faulted) {
            remaining.push_back(charger.vehicle->target - charger.vehicle->delivered);
        }
    }
    double elapsed = 0;
    for (const auto& vehicle : queue) {
        while (remaining.size() >= slots) {
            const double next = *std::min_element(remaining.begin(), remaining.end());
            const double power = std::min(chargerMax, limit() / remaining.size());
            elapsed += next / power * 60;
            for (auto& energy : remaining) energy -= next;
            remaining.erase(std::remove_if(remaining.begin(), remaining.end(),
                [](double energy) { return energy <= 1e-9; }), remaining.end());
        }
        estimates.push_back(elapsed);
        remaining.push_back(vehicle->target - vehicle->delivered);
    }
    return estimates;
}

std::string Station::command(const std::string& input) {
    std::istringstream args(input);
    std::string action;
    args >> action;
    Lock lock(mutex_);

    if (action == "status" && atEnd(args)) return snapshot();
    if (action == "add") {
        if (demoActive_) return errorJson("Stop demo traffic before adding a manual request.");
        std::string plate, service;
        int soc;
        double target;
        if (!(args >> plate) || !readNumber(args, soc) || !(args >> service)
            || !readNumber(args, target) || !atEnd(args) || !validPlate(plate)
            || soc < 0 || soc > 99 || !std::isfinite(target) || target < 0.1 || target > 100
            || (service != "normal" && service != "reserved" && service != "premium"
                && service != "fleet" && service != "emergency" && service != "medic")) {
            return errorJson("Use add PLATE SOC SERVICE KWH. SOC 0–99, energy 0.1–100 kWh.");
        }
        if (queue_.size() >= maxQueue) return errorJson("Waiting queue is full.");
        std::transform(plate.begin(), plate.end(), plate.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        auto waiting = queued();
        if (std::any_of(waiting.begin(), waiting.end(), [&](auto& v) { return v->plate == plate; })
            || std::any_of(chargers_.begin(), chargers_.end(), [&](const auto& c) {
                return c.vehicle && c.vehicle->plate == plate;
            })) return errorJson("This vehicle already has an open request.");
        const int id = enqueue(plate, soc, service, target);
        pthread_cond_broadcast(&changed_);
        return "{\"ok\":true,\"id\":" + std::to_string(id) + "}";
    }
    if (action == "cap" || action == "peak" || action == "settings") {
        double capacity = config_.capacity;
        std::string mode = autoPeak_ ? "auto" : peak_ ? "on" : "off";
        if (action != "peak" && (!readNumber(args, capacity) || !std::isfinite(capacity)
            || capacity < 5 || capacity > 500)) {
            return errorJson("Set a capacity between 5 and 500 kW.");
        }
        if (action != "cap" && (!(args >> mode) || (mode != "on" && mode != "off" && mode != "auto"))) {
            return errorJson("Choose peak on, peak off or peak auto.");
        }
        if (!atEnd(args)) return errorJson("Remove extra arguments from the power settings.");
        updateEnergy(Clock::now());
        config_.capacity = capacity;
        autoPeak_ = mode == "auto";
        peak_ = mode == "on" || (autoPeak_ && isPeakHour());
        rebalance();
        event("power", "Power settings updated · effective cap " + std::to_string(std::lround(limit())) + " kW");
        pthread_cond_broadcast(&changed_);
        return "{\"ok\":true}";
    }
    if (action == "fault" || action == "repair") {
        int id;
        if (!readNumber(args, id) || !atEnd(args) || id < 1 || id > config_.chargers)
            return errorJson("Choose a charger ID displayed in the roster.");
        auto& charger = chargers_[id - 1];
        if (action == "fault") {
            if (charger.faulted) return errorJson("Charger is already offline.");
            markFault(charger, Clock::now());
        } else {
            if (!charger.faulted) return errorJson("Charger is already online.");
            if (charger.vehicle) return errorJson("Session is still being released. Retry shortly.");
            repair(charger);
            pthread_cond_broadcast(&changed_);
        }
        return "{\"ok\":true}";
    }
    if (action == "demo" && atEnd(args)) {
        if (demoActive_ || workers_ || !queue_.empty()) return errorJson("Demo traffic requires an empty station.");
        demoActive_ = true;
        event("station", "Demo started at 10× with priority aging");
        enqueue("EV-1842", 48, "normal", 18);
        enqueue("EV-3920", 55, "normal", 14);
        enqueue("EV-1407", 42, "normal", 21);
        enqueue("EV-2056", 35, "normal", 16);
        enqueue("MEDIC-12", 44, "medic", 17);
        enqueue("EV-0618", 9, "normal", 19);
        enqueue("EV-0521", 15, "normal", 22);
        enqueue("EV-7182", 17, "normal", 13);
        enqueue("EV-9354", 19, "normal", 15);
        pthread_cond_broadcast(&changed_);
        return "{\"ok\":true}";
    }
    if (action == "stop-demo" && atEnd(args)) {
        if (!demoActive_) return errorJson("Demo traffic is idle.");
        demoStopping_ = true;
        abortQueued();
        finishDemoIfIdle();
        pthread_cond_broadcast(&changed_);
        return "{\"ok\":true}";
    }
    return errorJson("Unknown command or extra arguments.");
}

std::string Station::snapshot() {
    const auto now = Clock::now();
    updateEnergy(now);
    promoteWaiting(now);
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    const auto waiting = queued();
    const auto waits = waitEstimates(waiting);
    out << "{\"ok\":true,\"capacity\":" << config_.capacity << ",\"limit\":" << limit()
        << ",\"power\":" << totalPower() << ",\"speed\":" << clockSpeed()
        << ",\"demoActive\":" << (demoActive_ ? "true" : "false")
        << ",\"demoStopping\":" << (demoStopping_ ? "true" : "false")
        << ",\"agingMinutes\":" << config_.agingMinutes
        << ",\"peak\":" << (peak_ ? "true" : "false")
        << ",\"peakMode\":" << quoted(autoPeak_ ? "auto" : peak_ ? "on" : "off")
        << ",\"completed\":" << completed_ << ",\"energy\":" << completedEnergy_
        << ",\"revenue\":" << revenue_ << ",\"faultCount\":" << faultCount_
        << ",\"elapsedAverage\":" << (completed_ ? totalElapsedMinutes_ / completed_ : 0)
        << ",\"uptime\":" << std::chrono::duration<double>(now - started_).count()
        << ",\"logError\":" << quoted(logError_);
    auto writeVehicle = [&](const Vehicle& v) {
        out << "\"id\":" << v.id << ",\"plate\":" << quoted(v.plate) << ",\"soc\":" << v.soc
            << ",\"priority\":" << quoted(priorities[v.priority]) << ",\"rank\":" << v.priority
            << ",\"target\":" << v.target << ",\"delivered\":" << v.delivered
            << ",\"interruptions\":" << v.interruptions
            << ",\"aged\":" << (v.aged ? "true" : "false");
    };
    out << ",\"chargers\":[";
    bool first = true;
    for (const auto& c : chargers_) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << c.id << ",\"maxKw\":" << chargerMax << ",\"kw\":" << c.kw
            << ",\"state\":" << quoted(c.faulted ? "Offline" : c.vehicle ? "Charging" : "Available")
            << ",\"repairIn\":" << (c.faulted ? std::max(0.0, std::chrono::duration<double>(c.repairAt - now).count()) : 0)
            << ",\"vehicle\":";
        if (c.vehicle) {
            out << '{';
            writeVehicle(*c.vehicle);
            out << '}';
        } else out << "null";
        out << '}';
    }
    out << "],\"queue\":[";
    for (size_t i = 0; i < waiting.size(); ++i) {
        if (i) out << ',';
        out << '{';
        writeVehicle(*waiting[i]);
        if (demoActive_ && waiting[i]->priority == normalPriority) {
            const double until = std::chrono::duration<double>(promotionAt(*waiting[i]) - now).count();
            out << ",\"promotionIn\":" << until * clockSpeed() / 60;
        }
        out << ",\"eta\":";
        if (waits[i] < 0) out << "null";
        else out << waits[i];
        out << '}';
    }
    out << "],\"receipts\":[";
    first = true;
    for (const auto& receipt : receipts_) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << receipt.id << ",\"plate\":" << quoted(receipt.plate)
            << ",\"energy\":" << receipt.kwh << ",\"minutes\":" << receipt.minutes
            << ",\"cost\":" << receipt.cost << ",\"interruptions\":" << receipt.interruptions << '}';
    }
    out << "],\"events\":[";
    first = true;
    for (const auto& entry : events_) {
        if (!first) out << ',';
        first = false;
        out << "{\"time\":" << entry.time << ",\"type\":" << quoted(entry.type)
            << ",\"message\":" << quoted(entry.message) << '}';
    }
    out << "]}";
    return out.str();
}

std::string Station::dashboard() {
    Lock lock(mutex_);
    const auto now = Clock::now();
    updateEnergy(now);
    promoteWaiting(now);
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "VoltOS  |  EV charging station\n"
        << "Power " << totalPower() << " / " << limit() << " kW"
        << "  |  Peak " << (peak_ ? "on" : "off") << (autoPeak_ ? " (auto)" : "")
        << "  |  Clock " << clockSpeed() << "x";
    if (demoActive_) out << "  |  Demo aging " << config_.agingMinutes << " sim min";
    out << "\nCompleted " << completed_ << "  |  Billed energy " << completedEnergy_
        << " kWh  |  Revenue INR " << revenue_ << "  |  Faults " << faultCount_ << '\n';
    if (!logError_.empty()) out << "ALERT " << logError_ << '\n';
    out << "\nCHARGERS\n";
    for (const auto& c : chargers_) {
        out << "  #" << c.id << "  " << (c.faulted ? "Offline" : c.vehicle ? "Charging" : "Available")
            << "  " << c.kw << " kW";
        if (c.vehicle) out << "  " << c.vehicle->plate << "  " << c.vehicle->delivered
                           << " / " << c.vehicle->target << " kWh";
        if (c.faulted) out << "  recovery in " << std::max(0.0, std::chrono::duration<double>(c.repairAt - now).count()) << " s";
        out << '\n';
    }
    const auto waiting = queued();
    const auto waits = waitEstimates(waiting);
    out << "\nPRIORITY QUEUE  (" << waiting.size() << " waiting)\n";
    for (size_t i = 0; i < waiting.size(); ++i) {
        const auto& vehicle = *waiting[i];
        out << "  " << i + 1 << ". " << vehicle.plate << "  " << priorities[vehicle.priority];
        if (vehicle.aged) out << " (aged up)";
        out << "  SOC " << waiting[i]->soc << "%  |  ETA ";
        if (waits[i] < 0) out << "pending recovery";
        else out << waits[i] << " sim min";
        out << '\n';
    }
    out << "\nRECENT EVENTS\n";
    for (size_t i = 0; i < std::min<size_t>(5, events_.size()); ++i) out << "  " << events_[i].message << '\n';
    if (!receipts_.empty()) {
        const auto& last = receipts_.front();
        out << "\nLATEST BILL  " << last.plate << "  " << last.kwh << " kWh + "
            << last.minutes << " charging min = INR " << last.cost << '\n';
    }
    return out.str();
}
