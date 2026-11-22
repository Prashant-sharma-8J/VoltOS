#pragma once

#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <pthread.h>
#include <semaphore.h>

class Station {
public:
    struct Config {
        int chargers = 4;
        double capacity = 48;
        double speed = 10;
        double agingMinutes = 30;
        bool autoPeak = false;
        std::filesystem::path logRoot = "logs";
    };

    explicit Station(Config config);
    ~Station();
    Station(const Station&) = delete;
    Station& operator=(const Station&) = delete;

    // Commands return a single JSON document for the pipe or terminal client.
    std::string command(const std::string& input);
    std::string dashboard();
    const std::filesystem::path& logDirectory() const { return logDirectory_; }

private:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    struct Vehicle {
        int id, soc, priority;
        std::string plate;
        double target;
        double delivered = 0;
        double chargeHours = 0;
        int interruptions = 0;
        Time arrived, updated, queuedSince;
        Clock::duration queuedTime{};
        bool aged = false;
    };

    struct Charger {
        Station* station;
        int id;
        std::shared_ptr<Vehicle> vehicle;
        double kw = 0;
        bool faulted = false;
        Time repairAt{};
    };

    struct ByPriority {
        bool operator()(const std::shared_ptr<Vehicle>& a,
                        const std::shared_ptr<Vehicle>& b) const;
    };

    struct Event {
        long long time;
        std::string type, message;
    };

    struct Receipt {
        int id;
        std::string plate;
        double kwh, minutes, cost;
        int interruptions;
    };

    struct FileCloser { void operator()(FILE* file) const { if (file) std::fclose(file); } };
    using Log = std::unique_ptr<FILE, FileCloser>;

    static void* schedulerEntry(void*);
    static void* workerEntry(void*);
    void schedule();
    void charge(Charger&);
    bool dispatch(Time now);
    void updateEnergy(Time now);
    void updateEnergy(Charger&, Time now);
    void queueVehicle(const std::shared_ptr<Vehicle>&, Time now);
    Time promotionAt(const Vehicle&) const;
    void promoteWaiting(Time now);
    void abortQueued();
    void finishDemoIfIdle();
    double clockSpeed() const;
    void rebalance();
    void markFault(Charger&, Time now);
    void repair(Charger&);
    void complete(const Vehicle&, Time now);
    void shutdownRecord(const Vehicle&);
    void event(const std::string& type, const std::string& message);
    double limit() const;
    double totalPower() const;
    void checkInvariant() const;
    bool isPeakHour() const;
    int enqueue(const std::string& plate, int soc, const std::string& service, double target);
    std::vector<std::shared_ptr<Vehicle>> queued() const;
    std::vector<double> waitEstimates(const std::vector<std::shared_ptr<Vehicle>>& queue) const;
    std::string snapshot();
    void pause(unsigned milliseconds);

    Config config_;
    pthread_mutex_t mutex_{};
    pthread_cond_t changed_{};
    pthread_t scheduler_{};
    sem_t* slots_ = SEM_FAILED;
    std::vector<Charger> chargers_;
    std::priority_queue<std::shared_ptr<Vehicle>,
        std::vector<std::shared_ptr<Vehicle>>, ByPriority> queue_;
    Time nextPromotion_ = Time::max();
    std::deque<Event> events_;
    std::deque<Receipt> receipts_;
    std::filesystem::path logDirectory_;
    Log sessions_, faults_, transactions_;
    std::string logError_;
    Time started_ = Clock::now();
    bool stopping_ = false, peak_ = false, autoPeak_ = false;
    bool demoActive_ = false, demoStopping_ = false;
    int nextId_ = 1, workers_ = 0, completed_ = 0, faultCount_ = 0;
    double completedEnergy_ = 0, revenue_ = 0, totalElapsedMinutes_ = 0;
};
