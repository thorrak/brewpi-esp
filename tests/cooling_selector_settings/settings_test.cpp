#include "shim.h"
#include <cassert>
#include <iostream>

using GlycolCooling::Algorithm;

std::string filesystem_root;
unsigned settings_writes = 0;
unsigned min_times_writes = 0;
TempControl tempControl;
ExtendedSettings extendedSettings;
MinTimes minTimes;
Display display;
PiLink piLink;
Logger Log;

FILE *fs_open(const char *filename, const char *mode) {
    if (mode[0] == 'w') {
        if (std::strcmp(filename, ExtendedSettings::filename) == 0) ++settings_writes;
        if (std::strcmp(filename, MinTimes::filename) == 0) ++min_times_writes;
    }
    return std::fopen((filesystem_root + filename).c_str(), mode);
}

static JsonDocument parse(const std::string &text) {
    JsonDocument doc;
    assert(!deserializeJson(doc, text));
    return doc;
}

static bool update(const std::string &text) {
    return processExtendedSettingsJson(parse(text), true);
}

static std::string snapshot() {
    JsonDocument doc;
    serveExtendedSettings(doc);
    std::string result;
    serializeJson(doc, result);
    return result;
}

static void write_saved(const std::string &text) {
    FILE *file = std::fopen((filesystem_root + ExtendedSettings::filename).c_str(), "w");
    assert(file);
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

static void verify_defaults_and_reload() {
    extendedSettings.loadFromFilesystem();
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PredictiveCoast);
    assert(!extendedSettings.glycol);
    write_saved(R"({"glycol":true,"invertTFT":true,"largeTFT":false,"resetScreenOnPin":true})");
    extendedSettings.loadFromFilesystem();
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PredictiveCoast);
    assert(extendedSettings.glycol && extendedSettings.invertTFT && extendedSettings.resetScreenOnPin);

    for (const char *name : {"pulse_dose", "predictive_coast"}) {
        const std::string request = std::string(R"({"glycolCoolingAlgorithm":")") + name + "\"}";
        assert(update(request));
        JsonDocument response;
        serveExtendedSettings(response);
        assert(response["extendedSettings"]["glycolCoolingAlgorithm"] == name);
        extendedSettings.setDefaults();
        extendedSettings.loadFromFilesystem();
        assert(std::strcmp(GlycolCooling::selectionName(extendedSettings.glycolCoolingAlgorithm), name) == 0);
        assert(extendedSettings.glycol && extendedSettings.invertTFT);
    }
    for (const char *invalid : {"null", "42", "true", "[]", "{}", "\"unknown\""}) {
        write_saved(std::string(R"({"glycol":true,"glycolCoolingAlgorithm":)") + invalid + "}");
        extendedSettings.loadFromFilesystem();
        assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PredictiveCoast);
        assert(extendedSettings.glycol);
    }
    assert(tempControl.mode_changes == 0);
    assert(display.calls == 0);
    assert(min_times_writes == 0);
}

static void verify_partial_and_idempotent_updates() {
    assert(update(R"({"glycolCoolingAlgorithm":"pulse_dose"})"));
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PulseDose);
    const std::string before = snapshot();
    const unsigned writes = settings_writes;
    const unsigned mode_changes = tempControl.mode_changes;
    const unsigned display_calls = display.calls;
    assert(update(R"({"glycolCoolingAlgorithm":"pulse_dose"})"));
    assert(update("{}"));
    assert(update(R"({"glycol":true,"largeTFT":false,"invertTFT":false,"resetScreenOnPin":false})"));
    assert(snapshot() == before);
    assert(settings_writes == writes);
    assert(tempControl.mode_changes == mode_changes);
    assert(display.calls == display_calls);
    assert(update(R"({"largeTFT":true})"));
    assert(extendedSettings.largeTFT);
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PulseDose);
    assert(settings_writes == writes + 1);
    assert(tempControl.mode_changes == mode_changes);

    assert(update(R"({"SETTINGS_CHOICE":2,"MIN_SWITCH_TIME":123})"));
    assert(minTimes.settings_choice == MIN_TIMES_CUSTOM);
    assert(minTimes.MIN_SWITCH_TIME == 123);
    assert(min_times_writes == 1);
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PulseDose);
}

static void verify_invalid_http_is_atomic() {
    const std::string before = snapshot();
    const unsigned writes = settings_writes;
    const unsigned timing_writes = min_times_writes;
    const unsigned mode_changes = tempControl.mode_changes;
    const unsigned display_calls = display.calls;
    for (const char *invalid : {"null", "0", "1.5", "true", "[]", "{}", "\"\"", "\"PULSE_DOSE\"", "\"unknown\""}) {
        assert(!update(std::string(R"({"glycol":false,"largeTFT":false,"MIN_SWITCH_TIME":1,"glycolCoolingAlgorithm":)") + invalid + "}"));
        assert(snapshot() == before);
    }
    for (const char *invalid : {
        R"({"glycolCoolingAlgorithm":"predictive_coast","glycol":null})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","largeTFT":1})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","invertTFT":"true"})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","resetScreenOnPin":[]})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","SETTINGS_CHOICE":3})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","SETTINGS_CHOICE":null})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","MIN_SWITCH_TIME":-1})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","MIN_SWITCH_TIME":65536})",
        R"({"glycolCoolingAlgorithm":"predictive_coast","MIN_SWITCH_TIME":1.5})",
        "null", "true", "[]", "42"
    }) {
        assert(!update(invalid));
        assert(snapshot() == before);
    }
    assert(settings_writes == writes && min_times_writes == timing_writes);
    assert(tempControl.mode_changes == mode_changes && display.calls == display_calls);
    assert(!extendedSettings.setGlycolCoolingAlgorithm(static_cast<Algorithm>(99)));
    assert(snapshot() == before);
}

static void verify_telnet() {
    CommandProcessor processor;
    const unsigned mode_changes = tempControl.mode_changes;
    for (const char *name : {"predictive_coast", "pulse_dose", "pulse_dose"}) {
        piLink.incoming = std::string(R"({"glycolCoolingAlgorithm":")") + name + "\"}";
        processor.processExtendedSettingsJson();
        assert(piLink.prefix == 'X');
        const JsonDocument reply = parse(piLink.outgoing);
        assert(reply["glycolCoolingAlgorithm"] == name);
        assert(std::strcmp(GlycolCooling::selectionName(extendedSettings.glycolCoolingAlgorithm), name) == 0);
        assert(tempControl.mode_changes == mode_changes);
    }
    const unsigned writes = settings_writes;
    const std::string before = snapshot();
    for (const char *invalid : {"null", "123", "true", "\"unknown\""}) {
        piLink.incoming = std::string(R"({"glycol":false,"largeTFT":false,"glycolCoolingAlgorithm":)") + invalid + "}";
        processor.processExtendedSettingsJson();
        assert(snapshot() == before);
        assert(parse(piLink.outgoing)["glycolCoolingAlgorithm"] == "pulse_dose");
        assert(settings_writes == writes && tempControl.mode_changes == mode_changes);
    }
    extendedSettings.setDefaults();
    extendedSettings.loadFromFilesystem();
    assert(extendedSettings.glycolCoolingAlgorithm == Algorithm::PulseDose);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    filesystem_root = argv[1];
    verify_defaults_and_reload();
    verify_partial_and_idempotent_updates();
    verify_invalid_http_is_atomic();
    verify_telnet();
    std::cout << "Cooling selector settings: defaults, persistence, partial HTTP updates, atomic rejection, and Telnet passed\n";
}
