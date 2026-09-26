// Native serialization/receiver contract fixture. Hardware setup is simulated;
// serializer functions and the manifest construction are injected from this branch.
#include <ArduinoJson.h>
#include "WaterTestCore.h"
#include "WaterTestProtocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using namespace WaterTestCore;
struct Sample { uint64_t address, conversion, read; int16_t raw; bool valid; };
JsonDocument manifest, terminal, bootList;
Program program;
uint64_t nativeNow = 1000000;
uint8_t bootIndex = 0;
bool appliedPump = false;
std::vector<Record> records;
char guid[17] = "DE00000000000077";
char currentBoot[37] = "610f857b-2877-43b2-a2a2-cfbd1df121e8";
uint64_t nowUs() { return nativeNow; }
bool physicalPump() { return appliedPump; }
void uuid(char* output) { std::strcpy(output, "fc73f39a-2222-4000-8888-000000000077"); }
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
    record.boot = bootIndex; record.seq = records.size();
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
    input["reported_input"]["volume_unit"] = "gal"; input["reported_input"]["water_volume"] = 5.0;
    input["reported_input"]["fermenter_capacity"] = 7.0; input["reported_input"]["temperature_unit"] = "F";
    input["reported_input"]["glycol_setpoint"] = nullptr;
    DeviceConfig beer{{{0x28,0,0,0,0,0,0,1},1,0,false}};
    DeviceConfig glycol{{{0x28,0,0,0,0,0,0,2},-1,0,false}};
    DeviceConfig cool{{{},0,26,true}};
    bool bath = true;
    // @@MANIFEST_SOURCE@@
}
void sample(bool beer, int16_t raw, bool validSample) {
    // Exercise the production sample-record builder injected at build time.
    Sample sample{0, nativeNow-750000, nativeNow, raw, validSample};
    nativeNow += 1000; // Record assembly follows the actual read.
    append(sampleRecord(sample, beer));
    if (beer) program.sample(sample.read/1e6, raw/16.0+1/16.0, validSample, nowUs()/1e6);
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
    makeManifest();
    assert(program.start(nowUs()/1e6,22.0625,minimumOn(),minimumOff()));
    Record boot{}; boot.kind=0;boot.code=static_cast<uint8_t>(Reason::Start);append(boot);
    Record clock{};clock.kind=1;clock.read_us=1790416800000000ULL;append(clock);
    recordOutput(true,false,false,Reason::Start);recordOutput(false,false,false,Reason::Start);recordPhase();
    for (unsigned elapsed=1;program.active()&&elapsed<maximumSeconds;++elapsed) {
        nativeNow=1000000ULL+elapsed*1000000ULL;
        if(elapsed%2==0) {
            // This is illustrative input, never evidence of physical accuracy.
            int16_t raw=static_cast<int16_t>(std::lround((22.0-std::max(0.,double(elapsed)-300)*.00025)*16));
            sample(true,raw,true);
            int16_t bathRaw=static_cast<int16_t>(std::lround((7.5+.4*std::sin(elapsed/300.0))*16));
            sample(false,bathRaw,elapsed!=306); // Invalid bath conversion during pilot pulse.
        }
        auto previousPhase=program.phase;
        program.tick(nowUs()/1e6);
        if(appliedPump!=program.pump) {
            appliedPump=program.pump;recordOutput(true,program.pump,true,program.reason);
        }
        if(previousPhase!=program.phase&&program.active()) recordPhase();
    }
    assert(!program.active()); assert(program.outcome==End::Completed);assert(program.pulse==3);
    recordOutput(true,false,false,program.reason);recordOutput(false,false,false,program.reason);recordPhase();
    // @@FINISH_SOURCE@@
    bounds[currentBoot]=static_cast<int64_t>(records.size())-1;
    outputRequest("put","",manifest);
    const size_t batchSize=12;
    for(size_t next=0;next<records.size();next+=batchSize) {
        JsonDocument request;common(request);auto list=request["records"].to<JsonArray>();
        size_t end=std::min(records.size(),next+batchSize);
        for(size_t i=next;i<end;++i) {assert(valid(records[i]));WaterTestProtocol::recordToJson(list.add<JsonObject>(),records[i],currentBoot,manifest["sensors"][records[i].role?"glycol":"beer"]["calibration_offset_c"]|0.0);}
        request["first_seq"]=records[next].seq;request["last_seq"]=records[end-1].seq;
        std::string batchId=WaterTestProtocol::batchIdentifier(manifest["test_id"].as<std::string>(),next);
        request["batch_id"]=batchId;request["boot_id"]=currentBoot;
        outputRequest("post","/batches",request);
    }
    outputRequest("put","/finish",terminal);
}
