#include "GlycolMode.h"
#include "Ticks.h"
#include "ChamberMode.h"
#include "GlycolLog.h"
#include "ESPEepromAccess.h"
#include "PiLink.h"
#include "thorlog.h"
#include <fstream>
#include <functional>
#include <string>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>

HostTicks ticks;
HostPiLink piLink;
HostLog Log;
MinTimes minTimes;
ValueActuator cameraLightState;
std::function<void()> onFileAccess;
bool failFileRemoval = false;
FILE* fs_open(const char* name, const char* mode) {
    if (onFileAccess) onFileAccess();
    return std::fopen((std::string(FS_PREFIX) + name).c_str(), mode);
}
bool fs_exists(const char* name) {
    std::ifstream file(std::string(FS_PREFIX) + name);
    return file.good();
}
bool fs_remove(const char* name) {
    return !failFileRemoval && std::remove((std::string(FS_PREFIX) + name).c_str()) == 0;
}
// The chamber state machine is outside these glycol lifecycle tests.
void ChamberMode::updateState(Context& ctx, bool stayIdle) {
    assert(stayIdle);
    ctx.state = IDLE;
}

TempControl tempControl;
HostExtendedSettings extendedSettings;
ValueActuator defaultActuator;
void JSONSaveable::writeJsonToFile(const char*, const JsonDocument&) {}
JsonDocument JSONSaveable::readJsonFromFile(const char*) { return JsonDocument(); }
static char savedMode = Modes::off;
void ControlSettings::storeToFilesystem() {
    ++tempControl.settingsWrites;
    tempControl.storedWithOutputActive = tempControl.cooler->isActive() ||
        tempControl.heater->isActive() || tempControl.light->isActive();
    savedMode = mode;
}

static temperature q9(double c) { return static_cast<temperature>(std::lround(c * 512 + C_OFFSET)); }
class Input : public BasicTempSensor {
public:
    temperature value = q9(21.5);
    bool connected = true;
    bool fail_read = false;
    int reads = 0;
    bool isConnected() override { return connected; }
    bool init() override { return connected; }
    temperature read() override { ++reads; return connected && !fail_read ? value : INVALID_TEMP; }
};
struct Fixture {
    ControlConstants cc;
    ControlSettings cs;
    ControlVariables cv{};
    MinTimes times;
    GlycolConfig config;
    GlycolRuntimeState runtime{};
    Input input;
    Input glycol_input;
    TempSensor sensor{TEMP_SENSOR_TYPE_BEER, &input};
    TempSensor glycol_sensor{TEMP_SENSOR_TYPE_FRIDGE, &glycol_input};
    ValueActuator heater, cooler, light;
    uint8_t state = IDLE;
    uint16_t idle_time = 0, heat_time = 0, cool_time = 0, wait_time = 0;
    unsigned char integral_counter = 0;
    bool heater_present = false;
    GlycolCooling::Algorithm selection = GlycolCooling::Algorithm::PredictiveCoast;
    Fixture() {
        runtime.reset();
        cs.beerSetting = q9((70.5-32)/1.8);
        sensor.setFastFilterCoefficients(3);
        sensor.setSlowFilterCoefficients(4);
        sensor.setSlopeFilterCoefficients(4);
        cc.tempFormat = 'F';
        times.MIN_SWITCH_TIME = 60;
    }
    ControlContext context() {
        return {cc, cs, cv, times, &sensor, &glycol_sensor,
                heater_present ? static_cast<Actuator*>(&heater) : &defaultActuator,
                &cooler, &defaultActuator, state, idle_time, heat_time, cool_time, wait_time};
    }
    bool pump() const { return state == COOLING || state == COOLING_MIN_TIME; }
    bool heat() const { return state == HEATING || state == HEATING_MIN_TIME; }
    void update(uint64_t ms, bool sample = true) {
        ticks.now_ms = ms;
        tempControl.cc = cc;
        extendedSettings.glycolCoolingAlgorithm = selection;
        if (sample) sensor.update();
        auto c = context();
        GlycolMode::Context ctx(c, config, runtime, selection);
        cv.beerDiff = cs.beerSetting - sensor.readSlowFiltered();
        cv.beerSlope = sensor.readSlope();
        GlycolMode::updateHeatingPID(ctx, integral_counter);
        GlycolMode::updateState(ctx);
        cooler.setActive(pump()); heater.setActive(heat());
    }
    void suspend(uint64_t ms) {
        ticks.now_ms = ms;
        auto c = context();
        GlycolMode::Context ctx(c, config, runtime, selection);
        GlycolMode::suspend(ctx);
        cooler.setActive(false); heater.setActive(false);
    }
};


static void attach(TempControl& control, Fixture& fixture, ValueActuator& fan) {
    control = TempControl{};
    control.cc = fixture.cc;
    control.cs = fixture.cs;
    control.beerSensor = &fixture.sensor;
    control.fridgeSensor = &fixture.glycol_sensor;
    control.cooler = &fixture.cooler;
    control.heater = &fixture.heater;
    control.light = &fixture.light;
    control.fan = &fan;
    control.glycolRuntime.reset();
    minTimes = fixture.times;
    extendedSettings.glycol = true;
    extendedSettings.glycolCoolingAlgorithm = fixture.selection;
}

static void manualLifecycleChecks() {
    for (auto algorithm : {GlycolCooling::Algorithm::PredictiveCoast,
                           GlycolCooling::Algorithm::PulseDose}) {
        for (bool manual_already_off : {false, true}) {
            for (bool restart_heater : {false, true}) {
                Fixture f; ValueActuator fan;
                f.selection = algorithm;
                attach(tempControl, f, fan);
                auto& control = tempControl;
                minTimes.MIN_SWITCH_TIME = 5;
                minTimes.MIN_HEAT_OFF_TIME = 7;
                control.cc.lightAsHeater = restart_heater;
                control.cs.mode = Modes::test;
                control.cs.fridgeSetting = control.cs.beerSetting = INVALID_TEMP;
                f.input.connected = f.glycol_input.connected = false;
                // DeviceManager's manual output commands do not require sensors.
                f.cooler.setActive(true); f.heater.setActive(true); f.light.setActive(true);
                fan.setActive(true);
                for (unsigned i = 0; i < 3; ++i) {
                    ticks.now_ms = 1000000 + 1000 * i;
                    control.updateState(); control.updateOutputs();
                    assert(f.cooler.isActive() && f.heater.isActive() && f.light.isActive());
                    assert(fan.isActive());
                }
                if (manual_already_off) {
                    f.cooler.setActive(false); f.heater.setActive(false); f.light.setActive(false);
                }
                // Handoff must protect even an OFF edge made just before exit.
                ticks.now_ms = 1010000;
                control.setMode(Modes::beerConstant);
                assert(!f.cooler.isActive() && !f.heater.isActive());
                if (control.cc.lightAsHeater) assert(!f.light.isActive());
                assert(!control.storedWithOutputActive);
                assert(control.glycolRuntime.last_pump_active_s == 1010);
                assert(control.glycolRuntime.last_heater_active_s == 1010);
                assert(!control.glycolRuntime.cooling_output.pump_on);
                f.input.connected = true;
                f.input.value = q9(restart_heater ? 19 : 25);
                f.sensor.init(); f.sensor.update();
                control.cs.beerSetting = q9(21);
                control.glycolRuntime.heating_output = control.cc.pidMax_heat;
                for (unsigned elapsed = 0; elapsed <= 7; ++elapsed) {
                    ticks.now_ms = 1010000 + elapsed * 1000;
                    control.updateState(); control.updateOutputs();
                    if (!restart_heater) {
                        assert(f.cooler.isActive() == (elapsed >= 5));
                    } else {
                        assert(f.light.isActive() == (elapsed >= 7));
                        assert(!f.heater.isActive());
                    }
                }
                // Entering test mode also turns automatic outputs OFF before save.
                ticks.now_ms += 500;
                control.setMode(Modes::test);
                assert(!f.cooler.isActive() && !f.heater.isActive());
                if (control.cc.lightAsHeater) assert(!f.light.isActive());
                assert(!control.storedWithOutputActive);
            }
        }
    }
    std::puts("manual relay ownership and guarded automatic handoff checks passed");
}

static void tuningLifecycleChecks() {
    std::remove("glycolTuning.json");
    Fixture fixture; ValueActuator fan;
    attach(tempControl, fixture, fan);
    auto& control = tempControl;
    control.cs.mode = Modes::beerConstant;
    control.storeSettings();
    const auto savedControl = control.cs;
    auto learned = control.glycolRuntime.cooling.tuning();
    learned.predictive = {720, 0.025, 8, 9};
    learned.pulse_dose = {0.065, 12};
    assert(control.glycolRuntime.cooling.restoreTuning(learned));

    unsigned accesses = 0;
    onFileAccess = [&]() {
        ++accesses;
        assert(!fixture.cooler.isActive() && !fixture.heater.isActive());
        assert(!control.cc.lightAsHeater || !fixture.light.isActive());
    };
    control.state = COOLING;
    control.updateOutputs();
    assert(fixture.cooler.isActive());
    assert(!fs_exists(GlycolTuningStore::filename));
    control.state = HEATING;
    control.updateOutputs();
    assert(fixture.heater.isActive());
    assert(!fs_exists(GlycolTuningStore::filename));
    control.cc.lightAsHeater = true;
    control.updateOutputs();
    assert(fixture.light.isActive() && !fixture.heater.isActive());
    assert(!fs_exists(GlycolTuningStore::filename));
    control.state = IDLE;
    control.updateOutputs();
    assert(fs_exists(GlycolTuningStore::filename));
    assert(accesses > 0);
    accesses = 0;
    control.updateOutputs();
    assert(accesses == 0);
    onFileAccess = {};

    // A test's temporary OFF mode cannot replace the saved normal mode.
    WaterTest::owned = true;
    control.cs.mode = Modes::off;
    control.storeSettings();
    assert(control.settingsWrites == 1 && savedMode == savedControl.mode);
    ticks.now_ms = 500000;
    control.resumeAfterWaterTest(savedControl);
    WaterTest::owned = false;
    assert(control.cs.mode == savedControl.mode);
    assert(control.glycolRuntime.cooling.tuning().predictive.coast_s == 720);
    assert(std::fabs(control.glycolRuntime.cooling.tuning().pulse_dose.gain_c_per_on_s - 0.065) < 1e-15);
    assert(!control.glycolRuntime.cooling_output.pump_on);
    assert(control.glycolRuntime.last_pump_active_s == 500);

    // Startup loads both estimates with fresh control history and relay state.
    attach(tempControl, fixture, fan);
    extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    control.loadGlycolParams();
    assert(control.glycolRuntime.cooling.selection() == GlycolCooling::Algorithm::PulseDose);
    assert(control.glycolRuntime.cooling.tuning().predictive.coast_s == 720);
    assert(std::fabs(control.glycolRuntime.cooling.tuning().pulse_dose.gain_c_per_on_s - 0.065) < 1e-15);
    assert(!control.glycolRuntime.cooling_output.pump_on);
    assert(!control.glycolRuntime.clock_initialized && !control.glycolRuntime.cooling_step_initialized);
    std::remove("glycolTuning.json");
}

#ifdef ENABLE_GLYCOL_LOGGING
static std::string readLog(const char* filename) {
    std::ifstream file(filename);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
static void loggingChecks() {
    std::remove("glycol_log.csv"); std::remove("glycol_log.archived.csv");
    { std::ofstream old("glycol_log.csv"); old << "legacy,columns\nlegacy,event\n"; }
    glycolLog = GlycolLogger{};
    glycolLog.logReboot();
    assert(readLog("glycol_log.archived.csv") == "legacy,columns\nlegacy,event\n");
    assert(readLog("glycol_log.csv").find("algorithm,temperature_c,setpoint_c") != std::string::npos);
    Fixture f; ValueActuator fan; f.input.value = q9(25);
    attach(tempControl, f, fan);
    tempControl.cs.mode = Modes::beerConstant;
    f.sensor.init(); f.sensor.update();
    ticks.now_ms = 60000;
    auto before = readLog("glycol_log.csv");
    tempControl.updateState();
    assert(readLog("glycol_log.csv") == before); // decision does not write storage
    tempControl.updateOutputs();
    auto cooling = readLog("glycol_log.csv");
    assert(cooling.size() > before.size());
    assert(cooling.find("IDLE,FULL_COOLING,predictive-coast-v1,25.000000") != std::string::npos);
    tempControl.updateOutputs();
    assert(readLog("glycol_log.csv") == cooling); // unchanged state is not logged
    // Any logging during protective reset must see physical commands already OFF.
    onFileAccess = [&]() {
        assert(!f.cooler.isActive() && !f.heater.isActive() && !f.light.isActive() && !fan.isActive());
    };
    f.input.fail_read = true; f.sensor.update(); ticks.now_ms = 60500;
    tempControl.updateState(); tempControl.updateOutputs();
    onFileAccess = {};
    auto fault = readLog("glycol_log.csv");
    const auto off = fault.find("FULL_COOLING,IDLE,predictive-coast-v1");
    assert(off != std::string::npos);
    assert(fault.substr(off).find(",0.000000000,0.500,") != std::string::npos);
    assert(fault.find("DISABLED_OR_SENSOR_FAULT") != std::string::npos);
    extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    ticks.now_ms = 61000;
    tempControl.updateState(); tempControl.updateOutputs();
    assert(readLog("glycol_log.csv").find("adaptive-pulse-dose-v1") != std::string::npos);
    // Failed legacy migration preserves both files and never appends new rows.
    { std::ofstream old("glycol_log.csv"); old << "legacy,columns\n"; }
    const auto archive = readLog("glycol_log.archived.csv");
    glycolLog = GlycolLogger{}; failFileRemoval = true;
    glycolLog.logReboot();
    assert(readLog("glycol_log.csv") == "legacy,columns\n");
    assert(readLog("glycol_log.archived.csv") == archive);
    failFileRemoval = false;
    glycolLog.clearLog();
    assert(!fs_exists("/glycol_log.archived.csv"));
    std::puts("optional transition logging, relay ordering and schema migration checks passed");
}
#endif

int main() {
    // Raw cache is one sensor read per update, never another bus read by control.
    {
        Fixture f;
        assert(f.sensor.readRawCached() == INVALID_TEMP);
        f.update(0);
        assert(f.input.reads == 1);
        assert(f.sensor.readRawCached() == f.input.value);
        f.input.value = q9(25);
        f.update(1000);
        assert(f.sensor.readRawCached() == q9(25));
        assert(f.sensor.readFastFiltered() != q9(25));
        f.input.fail_read = true;
        f.update(2000);
        assert(f.sensor.isConnected()); // reported connection alone is insufficient
        assert(f.sensor.readRawCached() == INVALID_TEMP && !f.pump());
        f.sensor.setSensor(&f.input);
        assert(f.sensor.readRawCached() == INVALID_TEMP);
    }
    // Boot inhibits before the core: no fictitious pulse, stale samples or learning.
    {
        Fixture f;
        for (unsigned s = 0; s < 60; ++s) {
            f.update(s*1000);
            assert(!f.pump());
            assert(f.runtime.cooling_output.learning_updates == 0);
            assert(f.runtime.cooling_output.actual_on_s == 0);
        }
        f.update(60000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        assert(std::abs(f.runtime.cooling_output.temperature_c - 21.5) < 1e-12);
        assert(f.runtime.pump_started_s == 60);
        auto gain = f.runtime.cooling_output.budget_gain_c_per_s;
        f.update(60500, false); // UI duplicate cannot consume another sample/second
        assert(f.runtime.cooling_last_step_ms == 60000);
        f.update(61000);
        assert(f.pump() && f.state == COOLING_MIN_TIME);
        f.update(62000);
        assert(!f.pump()); // small error produces the predictive minimum 2-second pulse
        assert(f.runtime.cooling_output.actual_on_s == 2);
        assert(f.runtime.cooling_output.budget_gain_c_per_s == gain);
    }
    // Near-target blind cooling uses half the error divided by the response gain.
    {
        Fixture f; f.input.value = q9(21.6875);
        f.update(60000); assert(f.pump());
        assert(!f.runtime.cooling_output.full_cooling);
        const auto expected = 0.5 * (21.6875 - (f.cs.beerSetting - C_OFFSET) / 512.0) / 0.03;
        assert(std::abs(f.runtime.cooling_output.pulse_budget_s - expected) < 1e-12);
        for (unsigned s=61;s<65;++s) { f.update(s*1000); assert(f.pump()); }
        f.update(65000); assert(!f.pump());
        assert(f.runtime.cooling_output.actual_on_s == 5);
    }
    // Far targets allow sustained demand immediately. There is no exploratory
    // pulse, fixed emergency dwell or legacy maximum-run cutoff.
    {
        Fixture f; f.input.value = q9(25);
        for (unsigned s=60;s<2800;++s) {
            f.update(s*1000);
            assert(f.pump() && f.runtime.cooling_output.full_cooling);
            assert(std::isinf(f.runtime.cooling_output.pulse_budget_s));
        }
        f.input.value = q9(21);
        f.update(2800000); assert(!f.pump()); // raw-temperature stop remains effective
        assert(f.runtime.cooling_output.phase == GlycolCooling::Phase::Coast);
    }
    // The measured cooling trend stops sustained demand above the target when
    // its projected coast endpoint reaches the setpoint.
    {
        Fixture f; f.input.value = q9(24);
        f.update(60000); assert(f.pump());
        bool stopped = false;
        for (unsigned s=61;s<1000;++s) {
            f.input.value = q9(24 - (s-60)*0.003);
            f.update(s*1000);
            if (!f.pump()) {
                assert(f.runtime.cooling_output.temperature_c > (f.cs.beerSetting-C_OFFSET)/512.0);
                assert(f.runtime.cooling_output.predicted_endpoint_c <= (f.cs.beerSetting-C_OFFSET)/512.0);
                stopped = true;
                break;
            }
        }
        assert(stopped);
    }
    // Display units cannot change cooling output.
    {
        Fixture a, b;
        a.cc.tempFormat = 'C'; b.cc.tempFormat = 'F';
        for (unsigned s=0;s<1000;++s) {
            a.input.value=b.input.value=q9(21.6875 - std::min(0.2, s*0.00025));
            a.update(s*1000); b.update(s*1000);
            assert(a.pump()==b.pump());
            assert(a.runtime.cooling_output.phase==b.runtime.cooling_output.phase);
            assert(a.runtime.cooling_output.budget_gain_c_per_s==b.runtime.cooling_output.budget_gain_c_per_s);
        }
    }
    // Immediate faults/mode OFF preserve the actual OFF edge through repeated resets.
    {
        Fixture f; f.update(60000); assert(f.pump());
        f.suspend(60500); assert(!f.pump());
        assert(f.runtime.t_pump_off == 60500);
        f.suspend(61000); assert(f.runtime.t_pump_off == 60500);
        f.update(61500); assert(!f.pump());
        f.update(62500); assert(f.pump()); // 2 seconds since actual OFF
        f.input.fail_read=true;
        f.update(62600); assert(!f.pump()); // fault need not wait minimum ON
    }
    // A setpoint change cannot cause a sub-2-second pulse; a same-second call is held.
    {
        Fixture f; f.update(60000); assert(f.pump());
        f.cs.beerSetting=q9(25);
        f.update(60500,false); assert(f.pump());
        f.update(61000); assert(f.pump());
        f.update(62000); assert(!f.pump());
        assert(f.runtime.cooling_output.learning_updates==0);
    }
    // Both hardware counter wraps preserve real elapsed timing, without a new boot guard.
    for (uint64_t origin : {uint64_t(65535000), uint64_t(0xfffffc18)}) {
        Fixture f;
        for (uint64_t offset=0;offset<=5000;offset+=1000) {
            f.update(origin+offset);
            assert(f.runtime.clock_elapsed_ms==origin+offset);
        }
        assert(f.runtime.cooling_output.actual_on_s==2);
        assert(!f.pump());
    }
    // Heater direction interlock occurs before core; input history excludes heating.
    {
        Fixture f;
        f.state=HEATING; f.runtime.state=GLYCOL_HEATING;
        f.runtime.heating_output=0;
        f.update(70000); assert(!f.pump());
        for (unsigned s=71;s<130;++s) { f.update(s*1000); assert(!f.pump()); }
        f.update(130000); assert(f.pump());
        assert(f.runtime.pump_started_s==130);
    }
    // Heating uses the PID output and minimum window duration.
    {
        Fixture f; f.heater_present=true; f.input.value=q9(19);
        f.update(60000); f.update(61000);
        assert(f.heat());
        assert(f.runtime.heating_window_on_time_s>=10);
        auto count=f.integral_counter;
        f.update(61500,false); assert(f.integral_counter==count);
    }
    // The configured margin controls the heating start threshold.
    {
        Fixture a, b;
        JsonDocument config;
        a.config.toJson(config);
        assert(config.size() == 1);
        assert(config["trigger_margin"].as<float>() == 0.1f);
        b.config.trigger_margin = 0.5f;
        for (auto* f : {&a, &b}) {
            f->heater_present = true;
            f->cc.tempFormat = 'C';
            f->cs.beerSetting = q9(21);
            f->input.value = q9(20.75);
            f->update(60000); f->update(61000);
            assert(f->runtime.heating_output > 0);
        }
        assert(a.heat() && !b.heat());
        b.config.trigger_margin = 0.2f;
        b.update(62000); b.update(63000);
        assert(b.heat());
    }
    // Runtime selection waits for the actual ON minimum, then observes OFF
    // timing even when UI callbacks, mode inhibition or clock wrap intervene.
    for (uint64_t origin : {uint64_t(60000), uint64_t(0xfffffc18)}) {
        Fixture f; f.input.value=q9(25);
        f.update(origin); assert(f.pump());
        f.selection=GlycolCooling::Algorithm::PulseDose;
        f.update(origin+500,false); assert(f.pump());
        f.update(origin+1000); assert(f.pump());
        assert(f.runtime.cooling.switchPending());
        f.update(origin+2000); assert(!f.pump());
        assert(f.runtime.cooling.selection()==GlycolCooling::Algorithm::PulseDose);
        f.update(origin+3000); assert(!f.pump());
        f.update(origin+4000); assert(f.pump());
        assert(f.runtime.cooling_output.pulse_budget_s==30);
        assert(!f.runtime.cooling.switchPending());
        assert(f.runtime.cooling_output.learning_updates==0);
        f.suspend(origin+4500); assert(!f.pump());
        f.selection=GlycolCooling::Algorithm::PredictiveCoast;
        f.suspend(origin+4600); // mode/chamber inhibition applies the request OFF
        f.update(origin+5500); assert(!f.pump());
        f.update(origin+6500); assert(f.pump());
        assert(f.runtime.cooling_output.full_cooling);
    }
    // Both algorithms share immediate fault handling and the heating/boot gate.
    for (auto algorithm : {GlycolCooling::Algorithm::PredictiveCoast,
                           GlycolCooling::Algorithm::PulseDose}) {
        Fixture f; f.selection=algorithm; f.input.value=q9(25);
        f.update(60000); assert(f.pump());
        f.input.fail_read=true; f.update(60500); assert(!f.pump());
        f.input.fail_read=false; f.update(61500); assert(!f.pump());
        f.update(62500); assert(f.pump());
        f.suspend(63000);
        f.state=HEATING; f.runtime.state=GLYCOL_HEATING;
        f.update(70000); assert(!f.pump());
        f.update(129000); assert(!f.pump());
        f.update(130000); assert(f.pump());
        assert(f.runtime.cooling_output.learning_updates==0);
        assert(f.runtime.cooling_output.response_updates==0);
    }
    // Compile the actual diagnostic methods with real ArduinoJson. Diagnostics
    // expose the predictive identity and parameters in Celsius,
    // preserve null continuous budgets, and fit the Telnet response buffer.
    {
        Fixture f; f.input.value=q9(25); f.update(60000);
        tempControl.cs=f.cs;
        tempControl.cv=f.cv;
        tempControl.glycolRuntime=f.runtime;
        tempControl.beerSensor=&f.sensor;
        tempControl.fridgeSensor=&f.glycol_sensor;
        tempControl.heater=&f.heater;
        tempControl.cooler=&f.cooler;
        tempControl.light=&f.light;
        JsonDocument variables, constants;
        tempControl.getControlVariablesDoc(variables);
        tempControl.getControlConstantsDoc(constants);
        JsonObject p=variables["glycolCooling"];
        assert(std::strcmp(p["algorithm"].as<const char*>(), "predictive-coast-v1")==0);
        assert(std::strcmp(p["selection"].as<const char*>(), "predictive_coast")==0);
        assert(std::strcmp(p["requestedSelection"].as<const char*>(), "predictive_coast")==0);
        assert(!p["switchPending"].as<bool>());
        assert(p["pumpOn"].as<bool>() && p["coolerActive"].as<bool>());
        assert(!p["heaterActive"].as<bool>() && !p["lightActive"].as<bool>());
        assert(p["fullCooling"].as<bool>() && p["pulseBudgetSeconds"].isNull());
        assert(p["sensorValid"].as<bool>() && p["rawC"].as<double>()==25);
        assert(!p["glycolSensorValid"].as<bool>() && p["glycolRawC"].isNull());
        assert(f.glycol_input.reads==0); // glycol is optional and never sampled by the controller
        assert(p["coastSeconds"].as<double>()==300);
        assert(p["budgetGainCPerPumpSecond"].as<double>()==0.03);
        assert(p["responseUpdates"].as<unsigned>()==0 && p["learningUpdates"].as<unsigned>()==0);
        assert(p["minOnSeconds"].as<double>()==2 && p["minOffSeconds"].as<double>()==2);
        assert(p["gainCPerPumpSecond"].isNull() && variables["adaptiveCooling"].isNull());
        JsonObject config=constants["glycolCoolingConfig"];
        assert(config.size()==21); // identity + selection + all 19 frozen parameters
        assert(config["near_target_c"].as<double>()==1);
        assert(config["startup_pulse_s"].as<double>()==2);
        assert(config["maximum_blind_budget_s"].as<double>()==120);
        assert(config["observe_coast_s"].as<double>()==450);
        assert(config["initial_coast_s"].as<double>()==300);
        assert(config["max_coast_estimate_s"].as<double>()==1800);
        assert(config["max_observe_coast_s"].as<double>()==2400);
        assert(config["initial_gain_c_per_on_s"].isNull());
        // Also check learned values with long fractional/large integer output.
        auto& output=tempControl.glycolRuntime.cooling_output;
        output.coast_s=1234.56789012345;
        output.budget_gain_c_per_s=0.000123456789012345;
        output.rate_c_per_s=-0.00123456789012345;
        output.temperature_c=output.predicted_endpoint_c=21.123456789012345;
        output.pulse_budget_s=119.123456789012345;
        output.actual_on_s=4294967.12345;
        output.response_updates=output.learning_updates=0xffffffff;
        tempControl.glycolRuntime.clock_elapsed_ms=0xffffffffffffffffULL;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        const auto variablesSize=measureJson(variables);
        const auto constantsSize=measureJson(constants);
        assert(variablesSize+1<=2048 && constantsSize+1<=2048);
        std::printf("selectable cooling JSON: variables %zu bytes, constants %zu bytes (2048-byte buffer)\n",
                    variablesSize, constantsSize);
        f.input.fail_read=true; f.update(61000);
        tempControl.glycolRuntime=f.runtime;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        assert(!variables["glycolCooling"]["sensorValid"].as<bool>());
        assert(variables["glycolCooling"]["rawC"].isNull());
        assert(!variables["glycolCooling"]["pumpOn"].as<bool>());
        assert(variables["glycolCooling"]["sensorConnected"].as<bool>());
        extendedSettings.glycol=false;
        variables.clear(); constants.clear();
        tempControl.getControlVariablesDoc(variables);
        tempControl.getControlConstantsDoc(constants);
        assert(variables["glycolCooling"].isNull());
        assert(constants["glycolCoolingConfig"].isNull());
        extendedSettings.glycol=true;
    }
    // Pulse-dose uses the same envelope but reports only its own configuration
    // and learned gain. Unassigned outputs do not report the shared fan dummy.
    {
        Fixture f; f.selection=GlycolCooling::Algorithm::PulseDose;
        f.input.value=q9(25); f.update(60000);
        tempControl.cs=f.cs; tempControl.cv=f.cv; tempControl.glycolRuntime=f.runtime;
        tempControl.beerSensor=&f.sensor; tempControl.fridgeSensor=&f.glycol_sensor;
        tempControl.cooler=&f.cooler;
        tempControl.heater=tempControl.light=&defaultActuator;
        defaultActuator.setActive(true);
        JsonDocument variables, constants;
        tempControl.getControlVariablesDoc(variables); tempControl.getControlConstantsDoc(constants);
        JsonObject cooling=variables["glycolCooling"];
        assert(std::strcmp(cooling["algorithm"].as<const char*>(), "adaptive-pulse-dose-v1")==0);
        assert(std::strcmp(cooling["selection"].as<const char*>(), "pulse_dose")==0);
        assert(cooling["pumpOn"].as<bool>() && cooling["coolerActive"].as<bool>());
        assert(!cooling["heaterActive"].as<bool>() && !cooling["lightActive"].as<bool>());
        assert(cooling["coastSeconds"].isNull() && cooling["budgetGainCPerPumpSecond"].isNull());
        assert(cooling["responseUpdates"].isNull());
        assert(cooling["gainCPerPumpSecond"].as<double>()==0.03);
        JsonObject config=constants["glycolCoolingConfig"];
        assert(config.size()==21);
        assert(config["initial_probe_s"].as<double>()==4);
        assert(config["far_probe_s"].as<double>()==30);
        assert(config["saturation_dose_s"].as<double>()==900);
        assert(config["initial_coast_s"].isNull() && config["maximum_blind_budget_s"].isNull());
        assert(measureJson(variables)+1<=2048 && measureJson(constants)+1<=2048);
        // A newly saved request is visible before the next control tick.
        extendedSettings.glycolCoolingAlgorithm=GlycolCooling::Algorithm::PredictiveCoast;
        variables.clear(); tempControl.getControlVariablesDoc(variables);
        assert(variables["glycolCooling"]["switchPending"].as<bool>());
        assert(std::strcmp(variables["glycolCooling"]["requestedSelection"].as<const char*>(),
                           "predictive_coast")==0);
        defaultActuator.setActive(false);
    }
    manualLifecycleChecks();
#ifdef ENABLE_GLYCOL_LOGGING
    loggingChecks();
#endif
    tuningLifecycleChecks();
    std::puts("selectable cooling BrewPi integration checks passed");
}
