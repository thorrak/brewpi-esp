#include "GlycolTuning.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include "ESPEepromAccess.h"

namespace {
using GlycolCooling::Algorithm;
using GlycolCooling::Controller;
using GlycolCooling::Tuning;

constexpr uint32_t saveIntervalMs = 1800000;
const Tuning learned = {{612.3456789012345, 0.012345678901234567, 13, 17},
                        {0.023456789012345678, 19}};
const Tuning second = {{421.75, 0.025, 23, 29}, {0.0625, 31}};
const char* valid_json = R"({"version":1,"predictive":{"algorithm":"predictive-coast-v1","coast_s":600,"budget_gain_c_per_s":0.015625,"learning_updates":7,"response_updates":8},"pulse_dose":{"algorithm":"adaptive-pulse-dose-v1","gain_c_per_on_s":0.03125,"learning_updates":9}})";

std::string path() {
    return std::string(FS_PREFIX) + GlycolTuningStore::filename;
}

void close(double actual, double expected) {
    assert(std::isfinite(actual) && std::isfinite(expected));
    assert(std::abs(actual - expected) <= std::max(std::abs(actual), std::abs(expected)) * 1e-14);
}

void equal(const Tuning& actual, const Tuning& expected) {
    close(actual.predictive.coast_s, expected.predictive.coast_s);
    close(actual.predictive.budget_gain_c_per_s, expected.predictive.budget_gain_c_per_s);
    assert(actual.predictive.learning_updates == expected.predictive.learning_updates);
    assert(actual.predictive.response_updates == expected.predictive.response_updates);
    close(actual.pulse_dose.gain_c_per_on_s, expected.pulse_dose.gain_c_per_on_s);
    assert(actual.pulse_dose.learning_updates == expected.pulse_dose.learning_updates);
}

void clean() {
    TuningIO::reset();
    std::remove(path().c_str());
    std::remove((path() + ".tmp").c_str());
}

void write(const std::string& contents) {
    std::ofstream file(path(), std::ios::binary | std::ios::trunc);
    file << contents;
    file.close();
    assert(file.good());
}

std::string read() {
    std::ifstream file(path(), std::ios::binary);
    assert(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool exists(const std::string& filename) {
    std::ifstream file(filename);
    return file.good();
}

void rejects(const std::string& contents) {
    clean();
    write(contents);
    Controller controller;
    assert(controller.restoreTuning(second));
    GlycolTuningStore store;
    assert(!store.load(controller, 0));
    equal(controller.tuning(), second);
    assert(store.saveIfChanged(controller, 0));
    assert(TuningIO::writes == 0);
    assert(read() == contents);
}

template<typename Change>
void rejectsDocument(Change change) {
    ArduinoJson::JsonDocument document;
    assert(!deserializeJson(document, valid_json));
    change(document);
    std::string contents;
    serializeJson(document, contents);
    rejects(contents);
}

void roundtrip() {
    clean();
    Controller source;
    GlycolTuningStore writer;
    assert(writer.saveIfChanged(source, 0));
    assert(!exists(path()) && TuningIO::writes == 0);
    assert(!writer.load(source, 0));
    assert(writer.saveIfChanged(source, 1));
    assert(!exists(path()) && TuningIO::writes == 0);
    assert(source.restoreTuning(learned));
    assert(source.step(100, 25, 21).pump_on);
    assert(writer.saveIfChanged(source, 2));
    assert(TuningIO::writes == 1 && !exists(path() + ".tmp"));
    const std::string contents = read();
    assert(writer.saveIfChanged(source, 3));
    assert(TuningIO::writes == 1 && read() == contents);

    ArduinoJson::JsonDocument document;
    assert(!deserializeJson(document, contents));
    assert(document["version"].as<unsigned>() == 1);
    assert(std::string(document["predictive"]["algorithm"].as<const char*>()) == "predictive-coast-v1");
    assert(std::string(document["pulse_dose"]["algorithm"].as<const char*>()) == "adaptive-pulse-dose-v1");
    for (Algorithm algorithm : {Algorithm::PredictiveCoast, Algorithm::PulseDose}) {
        Controller restored(algorithm);
        GlycolTuningStore reader;
        assert(reader.load(restored, 0));
        equal(restored.tuning(), learned);
        assert(!restored.output().pump_on && restored.output().phase == GlycolCooling::Phase::Idle);
        assert(std::isnan(restored.output().temperature_c));
        assert(restored.output().pulse_budget_s == 0 && restored.output().actual_on_s == 0);
        assert(reader.saveIfChanged(restored, 4));
        assert(TuningIO::writes == 1 && read() == contents);
        const auto decision = restored.step(0, 21.5, 21);
        assert(decision.pump_on);
        const double gain = algorithm == Algorithm::PredictiveCoast ?
            learned.predictive.budget_gain_c_per_s : learned.pulse_dose.gain_c_per_on_s;
        close(decision.pulse_budget_s, 0.25 / gain);
    }

    assert(source.restoreTuning(second));
    assert(!writer.saveIfChanged(source, 4));
    assert(TuningIO::writes == 1 && read() == contents);
    assert(writer.saveIfChanged(source, 2 + saveIntervalMs));
    assert(TuningIO::writes == 2 && read() != contents);
    Controller restored;
    GlycolTuningStore reader;
    assert(reader.load(restored, 0));
    equal(restored.tuning(), second);
}

void invalidFiles() {
    rejects("");
    rejects("{");
    rejects("null");
    rejects("[]");
    rejects(std::string(1025, ' ') + valid_json);
    for (const char* nonfinite : {"1e999", "-1e999", "NaN", "Infinity"}) {
        std::string contents = valid_json;
        contents.replace(contents.find("600"), 3, nonfinite);
        rejects(contents);
    }
    rejectsDocument([](auto& doc) { doc.remove("version"); });
    rejectsDocument([](auto& doc) { doc["version"] = 2; });
    rejectsDocument([](auto& doc) { doc["version"] = "1"; });
    rejectsDocument([](auto& doc) { doc["version"] = true; });
    rejectsDocument([](auto& doc) { doc.remove("predictive"); });
    rejectsDocument([](auto& doc) { doc.remove("pulse_dose"); });
    rejectsDocument([](auto& doc) { doc["predictive"] = "invalid"; });
    rejectsDocument([](auto& doc) { doc["pulse_dose"] = nullptr; });
    for (const char* algorithm : {"predictive", "pulse_dose"}) {
        rejectsDocument([&](auto& doc) { doc[algorithm].remove("algorithm"); });
        rejectsDocument([&](auto& doc) { doc[algorithm]["algorithm"] = "unknown-v2"; });
        rejectsDocument([&](auto& doc) { doc[algorithm]["algorithm"] = 1; });
    }
    const char* number_paths[][2] = {
        {"predictive", "coast_s"}, {"predictive", "budget_gain_c_per_s"},
        {"pulse_dose", "gain_c_per_on_s"},
    };
    for (const auto& field : number_paths) {
        rejectsDocument([&](auto& doc) { doc[field[0]].remove(field[1]); });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = "0.03"; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = true; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = nullptr; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = -1; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = 0; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = std::numeric_limits<double>::infinity(); });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = std::numeric_limits<double>::quiet_NaN(); });
    }
    rejectsDocument([](auto& doc) { doc["predictive"]["coast_s"] = 89; });
    rejectsDocument([](auto& doc) { doc["predictive"]["coast_s"] = 1801; });
    rejectsDocument([](auto& doc) { doc["predictive"]["budget_gain_c_per_s"] = 0.00001; });
    rejectsDocument([](auto& doc) { doc["pulse_dose"]["gain_c_per_on_s"] = 0.00001; });
    rejectsDocument([](auto& doc) { doc["pulse_dose"]["gain_c_per_on_s"] = 0.51; });
    const char* counter_paths[][2] = {
        {"predictive", "learning_updates"}, {"predictive", "response_updates"},
        {"pulse_dose", "learning_updates"},
    };
    for (const auto& field : counter_paths) {
        rejectsDocument([&](auto& doc) { doc[field[0]].remove(field[1]); });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = -1; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = 0.5; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = uint64_t{1} << 32; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = "7"; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = true; });
        rejectsDocument([&](auto& doc) { doc[field[0]][field[1]] = nullptr; });
    }
}

void boundaryValues() {
    const uint32_t maximum_count = std::numeric_limits<uint32_t>::max();
    const Tuning boundaries[] = {
        {{90, 0.00005, 0, 0}, {0.00005, 0}},
        {{1800, 0.03, maximum_count, maximum_count}, {0.5, maximum_count}},
    };
    for (const auto& tuning : boundaries) {
        clean();
        Controller source;
        GlycolTuningStore writer;
        assert(source.restoreTuning(tuning));
        assert(writer.saveIfChanged(source, 0));
        Controller restored;
        GlycolTuningStore reader;
        assert(reader.load(restored, 0));
        equal(restored.tuning(), tuning);
    }
}

void interruptedWrite() {
    for (bool saved_file : {false, true}) {
        for (const char* temporary_contents : {"{", valid_json}) {
            clean();
            if (saved_file) {
                Controller source;
                GlycolTuningStore writer;
                assert(source.restoreTuning(second));
                assert(writer.saveIfChanged(source, 0));
            }
            const std::string prior = saved_file ? read() : "";
            {
                std::ofstream temporary(path() + ".tmp", std::ios::binary);
                temporary << temporary_contents;
                temporary.close();
                assert(temporary.good());
            }
            Controller restored;
            const Tuning defaults = restored.tuning();
            GlycolTuningStore reader;
            assert(reader.load(restored, 0) == saved_file);
            equal(restored.tuning(), saved_file ? second : defaults);
            assert(!exists(path() + ".tmp"));
            assert(!restored.output().pump_on);
            assert(reader.saveIfChanged(restored, 1));
            assert(TuningIO::writes == (saved_file ? 1u : 0u));
            if (saved_file) assert(read() == prior);
            else assert(!exists(path()));
        }
    }
}

void expectSaved(const Tuning& expected) {
    Controller restored;
    GlycolTuningStore reader;
    assert(reader.load(restored, 0));
    equal(restored.tuning(), expected);
}

void firstValueChange() {
    for (bool corrupt_file : {false, true}) {
        clean();
        if (corrupt_file) write("{");
        Controller controller;
        GlycolTuningStore store;
        assert(!store.load(controller, 1000));
        auto tuning = controller.tuning();
        tuning.predictive.learning_updates = 1;
        tuning.predictive.response_updates = 2;
        tuning.pulse_dose.learning_updates = 3;
        assert(controller.restoreTuning(tuning));
        assert(store.saveIfChanged(controller, 1001));
        assert(TuningIO::writes == 0);
        tuning.predictive.coast_s += 1;
        assert(controller.restoreTuning(tuning));
        assert(store.saveIfChanged(controller, 1002));
        assert(TuningIO::writes == 1);
        expectSaved(tuning);
    }

    clean();
    Controller controller;
    GlycolTuningStore store;
    assert(!store.load(controller, 0));
    auto tuning = controller.tuning();
    tuning.predictive.learning_updates = 100;
    tuning.predictive.response_updates = 200;
    tuning.pulse_dose.learning_updates = 300;
    assert(controller.restoreTuning(tuning));
    assert(store.saveIfChanged(controller, saveIntervalMs));
    assert(store.saveIfChanged(controller, 2 * saveIntervalMs));
    assert(TuningIO::writes == 0 && !exists(path()));
}

void individualValueChanges() {
    for (unsigned field = 0; field != 3; ++field) {
        clean();
        Controller controller;
        GlycolTuningStore store;
        assert(!store.load(controller, 0));
        auto tuning = controller.tuning();
        if (field == 0) tuning.predictive.coast_s += 1;
        if (field == 1) tuning.predictive.budget_gain_c_per_s *= 0.9;
        if (field == 2) tuning.pulse_dose.gain_c_per_on_s *= 0.9;
        assert(controller.restoreTuning(tuning));
        assert(store.saveIfChanged(controller, 1));
        assert(TuningIO::writes == 1);
        expectSaved(tuning);
    }
}

void coalescedChanges(uint32_t first_save) {
    clean();
    Controller controller;
    GlycolTuningStore store;
    assert(!store.load(controller, first_save));
    assert(controller.restoreTuning(learned));
    assert(store.saveIfChanged(controller, first_save));
    const std::string initial = read();

    assert(controller.restoreTuning(second));
    assert(!store.saveIfChanged(controller, first_save + 1));
    auto latest = second;
    latest.predictive.coast_s = 800;
    latest.predictive.budget_gain_c_per_s = 0.045;
    latest.pulse_dose.gain_c_per_on_s = 0.085;
    latest.predictive.learning_updates = 100;
    latest.predictive.response_updates = 200;
    latest.pulse_dose.learning_updates = 300;
    assert(controller.restoreTuning(latest));
    assert(!store.saveIfChanged(controller, first_save + saveIntervalMs - 1));
    assert(TuningIO::writes == 1 && read() == initial);
    assert(store.saveIfChanged(controller, first_save + saveIntervalMs));
    assert(TuningIO::writes == 2);
    expectSaved(latest);

    const std::string saved = read();
    latest.predictive.learning_updates += 1;
    latest.predictive.response_updates += 2;
    latest.pulse_dose.learning_updates += 3;
    assert(controller.restoreTuning(latest));
    assert(store.saveIfChanged(controller, first_save + saveIntervalMs + 1));
    assert(store.saveIfChanged(controller, first_save + 2 * saveIntervalMs));
    assert(TuningIO::writes == 2 && read() == saved);

    assert(controller.restoreTuning(second));
    assert(store.saveIfChanged(controller, first_save + 2 * saveIntervalMs + 1));
    assert(TuningIO::writes == 3);
    assert(controller.restoreTuning(latest));
    assert(!store.saveIfChanged(controller, first_save + 2 * saveIntervalMs + 2));
    auto returned = second;
    returned.predictive.response_updates += 100;
    assert(controller.restoreTuning(returned));
    assert(store.saveIfChanged(controller, first_save + 3 * saveIntervalMs + 1));
    assert(store.saveIfChanged(controller, first_save + 4 * saveIntervalMs));
    assert(TuningIO::writes == 3);
    expectSaved(second);
}

void rebootCooldown(uint32_t boot_ms) {
    clean();
    Controller source;
    GlycolTuningStore writer;
    assert(source.restoreTuning(learned));
    assert(writer.saveIfChanged(source, 0));

    Controller restored;
    GlycolTuningStore reader;
    assert(reader.load(restored, boot_ms));
    assert(restored.restoreTuning(second));
    assert(!reader.saveIfChanged(restored, boot_ms));
    assert(!reader.saveIfChanged(restored, boot_ms + saveIntervalMs - 1));
    assert(TuningIO::writes == 1);
    assert(reader.saveIfChanged(restored, boot_ms + saveIntervalMs));
    assert(TuningIO::writes == 2);
    expectSaved(second);
}

void firstSaveFailure() {
    clean();
    Controller controller;
    GlycolTuningStore store;
    assert(!store.load(controller, 0));
    assert(controller.restoreTuning(learned));
    TuningIO::fail_open = true;
    assert(!store.saveIfChanged(controller, 10));
    assert(TuningIO::writes == 1 && !exists(path()));
    TuningIO::fail_open = false;
    assert(!store.saveIfChanged(controller, 30009));
    assert(TuningIO::writes == 1);
    assert(store.saveIfChanged(controller, 30010));
    assert(TuningIO::writes == 2);
    assert(controller.restoreTuning(second));
    assert(!store.saveIfChanged(controller, 30010 + saveIntervalMs - 1));
    assert(TuningIO::writes == 2);
    assert(store.saveIfChanged(controller, 30010 + saveIntervalMs));
    assert(TuningIO::writes == 3);
}

void failedReplacement(bool& fault, uint32_t failed_at) {
    clean();
    Controller source;
    GlycolTuningStore writer;
    assert(source.restoreTuning(second));
    assert(writer.saveIfChanged(source, failed_at - saveIntervalMs));
    const std::string prior = read();
    assert(source.restoreTuning(learned));
    fault = true;
    assert(!writer.saveIfChanged(source, failed_at));
    assert(TuningIO::writes == 2 && read() == prior);
    fault = false;
    assert(!writer.saveIfChanged(source, failed_at + 29999));
    assert(TuningIO::writes == 2 && read() == prior);
    assert(writer.saveIfChanged(source, failed_at + 30000));
    assert(TuningIO::writes == 3 && read() != prior);
    assert(!exists(path() + ".tmp"));
    Controller restored;
    GlycolTuningStore reader;
    assert(reader.load(restored, 0));
    equal(restored.tuning(), learned);
    assert(writer.saveIfChanged(source, failed_at + 30001));
    assert(TuningIO::writes == 3);
    assert(source.restoreTuning(second));
    assert(!writer.saveIfChanged(source, failed_at + 30000 + saveIntervalMs - 1));
    assert(TuningIO::writes == 3);
    assert(writer.saveIfChanged(source, failed_at + 30000 + saveIntervalMs));
    assert(TuningIO::writes == 4);
}
}

int main() {
    invalidFiles();
    boundaryValues();
    roundtrip();
    interruptedWrite();
    firstValueChange();
    individualValueChanges();
    coalescedChanges(100);
    coalescedChanges(std::numeric_limits<uint32_t>::max() - saveIntervalMs / 2);
    rebootCooldown(1000);
    rebootCooldown(std::numeric_limits<uint32_t>::max() - saveIntervalMs / 2);
    firstSaveFailure();
    failedReplacement(TuningIO::fail_open, 100);
    failedReplacement(TuningIO::fail_sync, 100);
    failedReplacement(TuningIO::fail_rename, 100);
    failedReplacement(TuningIO::fail_rename, std::numeric_limits<uint32_t>::max() - 10000);
    clean();
    std::puts("Glycol tuning persistence: roundtrip, validation, write scheduling and atomic retry checks passed");
}
