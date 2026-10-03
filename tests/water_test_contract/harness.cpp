// Native serialization/receiver contract fixture. Hardware setup is simulated;
// serializer functions and the manifest construction are injected from this branch.
#include <ArduinoJson.h>
#include "WaterTestCore.h"
#include "WaterTestProtocol.h"
#include "GlycolCoolingController.h"
#include "WaterTestControllerSnapshot.h"
#include "WaterTestStorage.h"
#include "WaterTestUpload.h"
#include <array>
#include <memory>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using namespace WaterTestCore;
using namespace WaterTestStorage;
struct Sample { uint64_t address, conversion, read; int16_t raw; bool valid; };
JsonDocument manifest, terminal;
Program program;
Role recordedRole = Role::Baseline;
std::unique_ptr<GlycolCooling::Controller> testController;
bool controllerAllocationFailed = false;
GlycolCooling::Algorithm testAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
uint64_t denseUntilUs=0, lastControllerRecordUs=0, recordStartUs=1000000;
bool recordedControllerPump=false;
unsigned recordedControllerStarts=0, recordedControllerFinishes=0;
bool controllerCheckpointed[controllerRunCount] = {};
namespace WaterTestStorage {
JsonDocument checkpoints[controllerRunCount];
bool saveDocument(const char *path, const JsonDocument &doc) {
    checkpoints[std::strcmp(path, controllerOnePath) == 0 ? 0 : 1].set(doc);
    return true;
}
bool loadDocument(const char *path, JsonDocument &doc) {
    const auto &saved = checkpoints[std::strcmp(path, controllerOnePath) == 0 ? 0 : 1];
    if (saved.isNull()) return false;
    doc.set(saved); return true;
}
}
// @@RECORDING_CONSTANTS@@
uint64_t lastLoggedRead[2] = {}, nextSparseRead[2] = {};
uint32_t denseRecords = 0;
struct { GlycolCooling::Algorithm glycolCoolingAlgorithm=GlycolCooling::Algorithm::PredictiveCoast; } extendedSettings;
uint64_t nativeNow = 1000000;
bool appliedPump = false;
std::vector<Record> records;
char guid[17] = "DE00000000000077";
char currentBoot[37] = "610f857b-2877-43b2-a2a2-cfbd1df121e8";
char recordingBoot[37] = "610f857b-2877-43b2-a2a2-cfbd1df121e8";
uint64_t nowUs() { return nativeNow; }
bool physicalPump() { return appliedPump; }
void uuid(char* output) {
    std::strcpy(output, extendedSettings.glycolCoolingAlgorithm == GlycolCooling::Algorithm::PulseDose
        ? "fc73f39b-2222-4000-8888-000000000078" : "fc73f39a-2222-4000-8888-000000000077");
}
bool isNtpSynced() { return true; }
uint32_t minimumOn() { return 2; }
uint32_t minimumOff() { return 2; }
struct DeviceConfig { struct { uint8_t address[8]; int calibration; int pinNr; bool invert; } hw; };
constexpr int DEVICE_CHAMBER_HEAT = 1;
bool device(int, DeviceConfig&) { return false; }
namespace Config { namespace Version {
constexpr const char* release = "native-contract-test";
constexpr const char* git_sha = "actual-working-tree-serializer";
constexpr bool git_dirty = true;
} }
#define FIRMWARE_REVISION "native-contract-test"
#define CONTROLLER_TYPE "esp32-native-fixture"
struct { int cs = 0; struct { bool lightAsHeater = false; } cc; } tempControl;
int savedControl = 0;
void settingsToManifest(JsonObject o) { o["mode"] = "o"; o["fixture"] = true; }
bool append(Record record) {
    assert(records.size() < maxRecords);
    record.boot = 0; record.seq = records.size();
    record.t_us = std::max(record.t_us ? record.t_us : nativeNow, records.empty() ? uint64_t(0) : records.back().t_us);
    seal(record); assert(valid(record)); records.push_back(record); return true;
}
// @@SOURCE_FUNCTIONS@@
void makeManifest() {
    JsonDocument input;
    input["fermenter_model"] = "SYNTHETIC firmware serializer contract test";
    input["fermenter_capacity_l"] = 26.5; input["water_volume_l"] = 18.927;
    input["cooling_type"] = "immersion_coil"; input["probe_mounting"] = "thermowell";
    input["glycol_temperature_source"] = "chamber_probe"; input["reported_chiller_setpoint_c"] = nullptr;
    const bool dose = extendedSettings.glycolCoolingAlgorithm == GlycolCooling::Algorithm::PulseDose;
    input["glycol_flow_source"] = dose ? "measured_at_fermenter" : "pump_rating";
    input["glycol_flow_value"] = dose ? 2.25 : 200.5;
    input["glycol_flow_unit"] = dose ? "lpm" : "us_gph";
    input["reported_input"]["volume_unit"] = "gal"; input["reported_input"]["water_volume"] = 5.0;
    input["reported_input"]["fermenter_capacity"] = 7.0; input["reported_input"]["temperature_unit"] = "F";
    input["reported_input"]["glycol_setpoint"] = nullptr;
    DeviceConfig beer{{{0x28,0,0,0,0,0,0,1},1,0,false}};
    DeviceConfig glycol{{{0x28,0,0,0,0,0,0,2},-1,0,false}};
    DeviceConfig cool{{{},0,26,true}};
    bool bath = true;
    // @@MANIFEST_SOURCE@@
    recordingMetadata();
}
void sample(bool beer, int16_t raw, bool validSample) {
    // Exercise the production sample-record builder injected at build time.
    Sample sample{0, nativeNow-750000, nativeNow, raw, validSample};
    nativeNow += 1000; // Record assembly follows the actual read.
    const Record record = sampleRecord(sample, beer);
    const auto phase = program.phase;
    if (beer) program.sample(sample.read/1e6, raw/16.0+1/16.0, validSample, nowUs()/1e6);
    // The fixture receives every acquisition but retains only the declared
    // sparse/edge cadence, plus all qualifying final observation readings.
    // Queueing, cache freshness and durable writes belong to the backend suite.
    const unsigned role = beer ? 0 : 1;
    const bool regular = !nextSparseRead[role] || sample.read >= nextSparseRead[role];
    const bool dense = denseRecords < denseRecordBudget &&
        (!validSample || (sample.read <= denseUntilUs &&
                         (!lastLoggedRead[role] || sample.read - lastLoggedRead[role] >= denseSampleUs)));
    const bool finalObservation = beer && validSample && phase == Phase::ControllerFinalObserve &&
        program.controllerFinalOffConfirmed && !appliedPump &&
        sample.read / 1e6 > program.controllerFinalObservationStarted && sample.read <= nowUs();
    if (regular || dense || finalObservation || !program.active()) {
        append(record);
        lastLoggedRead[role] = sample.read;
        if (regular) nextSparseRead[role] = sample.read + sparseSampleUs;
        else if (dense) ++denseRecords;
    }
}
void outputRequest(const char* method, const char* suffix, const JsonDocument& payload) {
    JsonDocument request; request["method"] = method; request["suffix"] = suffix; request["payload"] = payload;
    std::string text; serializeJson(request,text); std::cout << text << '\n';
}
int main(int argc, char** argv) {
    if(argc==2 && std::string(argv[1])=="--ack") {
        std::string line;unsigned count=0;
        while(std::getline(std::cin,line)) {
            JsonDocument packet;assert(deserializeJson(packet,line)==DeserializationError::Ok);
            auto request=packet["request"]["payload"].as<JsonVariantConst>();
            auto response=packet["response"].as<JsonVariantConst>();
            std::string id=request["test_id"].as<std::string>();const char* device=request["device_guid"];
            auto suffix=packet["request"]["suffix"].as<std::string>();
            if(suffix=="/batches") assert(WaterTestProtocol::batchAcknowledged(response,id,device,request["batch_id"].as<std::string>(),request["first_seq"],request["last_seq"]));
            else if(suffix=="/finish") assert(WaterTestProtocol::finishAcknowledged(response,id,device));
            else assert(WaterTestProtocol::acknowledged(response,id,device));
            ++count;
        }
        std::cout<<"Actual firmware acknowledgement helpers accepted "<<count<<" portal responses.\n";return 0;
    }
    if (argc==2 && std::string(argv[1])=="--pulse-dose")
        extendedSettings.glycolCoolingAlgorithm=GlycolCooling::Algorithm::PulseDose;
    makeManifest();
    denseUntilUs = recordStartUs + edgeWindowUs;
    assert(program.start(nowUs()/1e6,22.0625,minimumOn(),minimumOff()));
    Record boot{}; boot.kind=0;boot.code=static_cast<uint8_t>(Reason::Start);append(boot);
    Record clock{};clock.kind=1;clock.read_us=1790416800000000ULL;append(clock);
    recordOutput(true,false,false,Reason::Start);recordOutput(false,false,false,Reason::Start);recordPhase();
    double water=22., rate=0.;
    bool badBathRecorded=false;
    for (unsigned elapsed=1;program.active()&&elapsed<maximumSeconds;++elapsed) {
        nativeNow=1000000ULL+elapsed*1000000ULL;
        // Known illustrative plant for serializer integration, not a physical calibration claim.
        rate += ((appliedPump ? .006 : 0.)-rate)/25.;
        water -= rate;
        if(elapsed%2==0) {
            sample(true,static_cast<int16_t>(std::lround(water*16)),true);
            int16_t bathRaw=static_cast<int16_t>(std::lround((7.5+.4*std::sin(elapsed/300.0))*16));
            const bool bad = appliedPump && !badBathRecorded;
            sample(false,bathRaw,!bad);
            badBathRecorded = badBathRecorded || bad;
        }
        auto previousPhase=program.phase;
        auto previousRole=program.role;
        program.tick(nowUs()/1e6);
        if(previousPhase==Phase::Controller && program.phase==Phase::Controller) {
            if(!testController) {
                PredictiveCooling::Config predictive; AdaptiveCooling::Config dose;
                predictive.min_on_s=dose.min_on_s=program.minimumOn;
                predictive.min_off_s=dose.min_off_s=program.minimumOff;
                testController.reset(new GlycolCooling::Controller(algorithmForRun(program.controllerRun),predictive,dose));
                testController->externalOff(program.switched);
            }
            const auto decision=testController->step(nowUs()/1e6,program.latestC,program.targetC,true);
            program.setControllerPump(nowUs()/1e6,decision.pump_on);
        }
        if(appliedPump!=program.pump) {
            appliedPump=program.pump;recordOutput(true,program.pump,true,program.reason);
        }
        if (program.phase == Phase::ControllerFinalObserve) {
            assert(!appliedPump);
            program.confirmControllerFinalOff(nowUs() / 1e6);
        }
        if((previousPhase!=program.phase || previousRole!=program.role)&&program.active()) recordPhase();
        if (program.controllerRun > recordedControllerStarts) {
            recordedControllerStarts = program.controllerRun;
            recordControllerRun(false);
        }
        recordControllerObservation();
        if (program.controllerRun && program.controllerRunEnded && program.controllerRun > recordedControllerFinishes) {
            closeControllerObservation();
            recordedControllerFinishes = program.controllerRun;
            recordControllerRun(true);
            assert(checkpointController());
        }
        if (program.phase == Phase::ControllerTransition && program.controllerTransitionReady) {
            assert(controllerCheckpointed[program.controllerRun - 1]);
            if (program.advanceController(nowUs()/1e6)) {
                testController.reset(); lastControllerRecordUs=0;
                recordedControllerObservation=0;
                recordPhase();
                recordedControllerStarts = program.controllerRun;
                recordControllerRun(false);
                recordControllerObservation();
            }
        }
        recordController();
    }
    assert(!program.active()); assert(program.outcome==End::Completed);assert(program.pulse>=4);
    recordOutput(true,false,false,program.reason);recordOutput(false,false,false,program.reason);recordPhase();
    // @@FINISH_SOURCE@@
    bounds[currentBoot]=static_cast<int64_t>(records.size())-1;
    outputRequest("put","",manifest);
    const size_t batchSize = WaterTestUpload::batchSize;
    for(size_t next=0;next<records.size();next+=batchSize) {
        JsonDocument request;common(request);auto list=request["records"].to<JsonArray>();
        size_t end=std::min(records.size(),next+batchSize);
        for(size_t i=next;i<end;++i) {assert(valid(records[i]));WaterTestProtocol::recordToJson(list.add<JsonObject>(),records[i],currentBoot,manifest["sensors"][records[i].role?"glycol":"beer"]["calibration_offset_c"]|0.0);}
        request["first_seq"]=records[next].seq;request["last_seq"]=records[end-1].seq;
        std::string batchId=WaterTestProtocol::batchIdentifier(manifest["test_id"].as<std::string>(),next);
        request["batch_id"]=batchId;request["boot_id"]=currentBoot;
        // Exercise the production bounded writer, comparing every field to
        // the original DOM layout before handing the real body to the portal.
        std::array<char, WaterTestUpload::workspaceBytes> workspace{};
        WaterTestUpload::Batch batch{guid, manifest["test_id"].as<const char *>(), batchId.c_str(), currentBoot,
            records.data() + next, end - next,
            manifest["sensors"]["beer"]["calibration_offset_c"] | 0.0,
            manifest["sensors"]["glycol"]["calibration_offset_c"] | 0.0};
        size_t measured = 0, written = 0;
        assert(WaterTestUpload::writeBatch(batch, workspace.data(), workspace.size(), nullptr, nullptr, measured));
        std::string body;
        auto sink = [](void *context, const char *data, size_t length) {
            static_cast<std::string *>(context)->append(data, length); return true;
        };
        assert(WaterTestUpload::writeBatch(batch, workspace.data(), workspace.size(), sink, &body, written));
        assert(measured == written && written == body.size());
        JsonDocument streamed;
        assert(deserializeJson(streamed, body) == DeserializationError::Ok);
        std::string legacyBody;
        serializeJson(request, legacyBody);
        JsonDocument legacyReceived;
        assert(deserializeJson(legacyReceived, legacyBody) == DeserializationError::Ok);
        assert(streamed.as<JsonVariantConst>() == legacyReceived.as<JsonVariantConst>());
        outputRequest("post","/batches",streamed);
    }
    outputRequest("put","/finish",terminal);
}
