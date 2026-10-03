int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "loop_normal" || scenario == "loop_fallback") {
        Native::failLoop = scenario == "loop_fallback";
        Native::stopAfterDelays = 3;
        if (!Native::failLoop) {
            app_main();
            assert(!RuntimeHealth::snapshot().loopStarted);
            assert(Native::loopTask != nullptr);
        }
        try {
            if (Native::failLoop) app_main();
            else Native::loopTask(nullptr);
            assert(false);
        } catch (const StopLoop&) {}
        const auto state = RuntimeHealth::snapshot();
        assert(state.loopStarted && state.loopIterations == 3 && Native::loopCalls == 3);
        assert(state.loopTaskFallback == Native::failLoop);
        assert(state.lastLoopUs == Native::now - 10000);
        return 0;
    }
    if (scenario == "health_read_only") {
        Native::failBus = true;
        assert(!ow_scanner.init(13));
        const unsigned before = Native::busCalls + Native::taskCalls + Native::busIo + Native::mutexCalls;
        JsonDocument json;
        health(json);
        assert(json["control_loop"]["started"] == false);
        assert(json["control_loop"]["last_tick_age_ms"].isNull());
        assert(json["onewire"]["running"] == false);
        assert(json["onewire"]["last_init_error"] == "bus_creation");
        assert(json["onewire"]["init_attempts"] == 1);
        assert(json["onewire"]["last_read_age_ms"].isNull());
        assert(json["network"]["http_clients"] == 2);
        Native::failHttpClientList = true;
        health(json);
        assert(json["network"]["http_clients"].isNull());
        assert(before == Native::busCalls + Native::taskCalls + Native::busIo + Native::mutexCalls);
        return 0;
    }
    OneWireScanner scanner;
    if (scenario == "live_worker") {
        assert(scanner.init(13));
        Native::stopAfterDelays = 4;
        try { Native::worker(Native::workerArgument); } catch (const StopLoop&) {}
        auto state = scanner.health();
        assert(state.running && state.enumerations == 1 && state.successfulReads == 2);
        assert(state.failedReads == 0 && state.lastSuccessfulReadUs > 0 && Native::samples == 2);
        Native::readValid = false;
        Native::stopAfterDelays = 6;
        try { Native::worker(Native::workerArgument); } catch (const StopLoop&) {}
        state = scanner.health();
        assert(state.successfulReads == 2 && state.failedReads == 1);
        Native::now += 120000000;
        scanner.retry_if_stopped(13);
        assert(Native::taskCalls == 1 && Native::busCalls == 1);
        assert(scanner.init(13));
        assert(Native::taskCalls == 1);
        return 0;
    }
    OneWireScanner::InitError expected;
    if (scenario == "mutex_failure") {
        Native::failMutex = true;
        expected = OneWireScanner::InitError::MutexAllocation;
    } else if (scenario == "bus_failure") {
        Native::failBus = true;
        expected = OneWireScanner::InitError::BusCreation;
    } else {
        assert(scenario == "worker_failure");
        Native::failWorker = true;
        expected = OneWireScanner::InitError::WorkerAllocation;
    }
    assert(!scanner.init(13));
    auto state = scanner.health();
    assert(!state.running && state.lastInitError == expected && state.initFailures == 1);
    if (scenario == "worker_failure") assert(Native::deletions == 1);
    Native::now += 29999999;
    for (unsigned n = 0; n < 100; ++n) scanner.retry_if_stopped(13);
    assert(scanner.health().initAttempts == 1);
    ++Native::now;
    scanner.retry_if_stopped(13);
    assert(scanner.health().initAttempts == 2 && scanner.health().initFailures == 2);
    Native::failMutex = Native::failBus = Native::failWorker = false;
    Native::now += 30000000;
    scanner.retry_if_stopped(13);
    state = scanner.health();
    assert(state.running && state.initAttempts == 3 && state.initFailures == 2);
    assert(state.lastInitError == OneWireScanner::InitError::None);
    assert(state.lastBusCreateError == ESP_OK);
}
