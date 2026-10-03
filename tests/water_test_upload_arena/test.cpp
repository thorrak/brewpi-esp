#include "WaterTestUpload.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

struct CountSink {
  size_t count = 0;
  bool fail = false;
  static bool write(void *context, const char *, size_t size) {
    auto &sink = *static_cast<CountSink *>(context);
    if (sink.fail) return false;
    sink.count += size;
    return true;
  }
};
int main() {
  using namespace WaterTestCore;
  // Native slots are larger than ESP32 slots; 64 native slots keep the same
  // 1024-byte pool allocation as the device's default 128 slots. This exercises
  // the actual 3072-byte workspace with a conservative per-field memory cost.
  constexpr size_t poolBytes = ARDUINOJSON_POOL_CAPACITY * ArduinoJson::detail::ResourceManager::slotSize;
  static_assert(poolBytes == 1024 || poolBytes == 2048);
  struct Workspace {
    unsigned char before[16];
    alignas(std::max_align_t) unsigned char bytes[3072];
    unsigned char after[16];
  } workspace;
  memset(&workspace, 0xa5, sizeof(workspace));
  static_assert(WaterTestUpload::workspaceBytes == 3072);
  Record records[WaterTestUpload::batchSize]{};
  WaterTestUpload::Batch batch{"45C40A2400005039", "a1234567-1234-4000-8000-123456789abc",
      "a1234567-1234-4000-8000-123456789abc-ffffffff",
      "b1234567-1234-4000-8000-123456789abc", records, WaterTestUpload::batchSize, 0.0625, -0.125};
  size_t cases = 0, largest = 0;
  for (unsigned kind = 0; kind <= 10; ++kind) {
    const unsigned codes = kind == 4 ? unsigned(Phase::ControllerFinalObserve) + 1 :
        (kind == 0 || kind == 3) ? unsigned(Reason::ControllerDurationComplete) + 1 : 3;
    for (unsigned code = 0; code < codes; ++code)
      for (unsigned role = 0; role <= 2; ++role)
        for (unsigned flags = 0; flags <= 255; ++flags) {
          for (unsigned i = 0; i < WaterTestUpload::batchSize; ++i) {
            auto &record = records[i];
            record = {};
            record.seq = UINT32_MAX - WaterTestUpload::batchSize + i + 1;
            record.t_us = UINT64_MAX;
            record.read_us = UINT64_MAX;
            record.conversion_us = UINT32_MAX;
            record.detail = UINT32_MAX;
            record.raw = i % 2 ? INT16_MIN : INT16_MAX;
            record.kind = kind;
            record.code = code;
            record.role = role;
            record.flags = flags;
            record.pulse = i % 3;
            if (kind == 9) {
              const double target = i % 2 ? -0.5555555555555556 : 19.444444444444443;
              uint64_t bits;
              memcpy(&bits, &target, sizeof(bits));
              record.conversion_us = uint32_t(bits);
              record.detail = uint32_t(bits >> 32);
            }
            seal(record);
          }
          size_t expected = 0, actual = 0;
          CountSink sink;
          assert(WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), nullptr, nullptr, expected));
          assert(WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, actual));
          assert(expected == actual && actual == sink.count);
          largest = std::max(largest, actual);
          ++cases;
          for (unsigned char byte : workspace.before) assert(byte == 0xa5);
          for (unsigned char byte : workspace.after) assert(byte == 0xa5);
        }
  }
  size_t ignored;
  CountSink sink;
  assert(!WaterTestUpload::writeBatch(batch, workspace.bytes, 512, CountSink::write, &sink, ignored));
  assert(sink.count == 0);
  assert(WaterTestUpload::writeBatch(batch, workspace.bytes + 1, sizeof(workspace.bytes) - 1,
                                    CountSink::write, &sink, ignored));
  for (unsigned char byte : workspace.before) assert(byte == 0xa5);
  for (unsigned char byte : workspace.after) assert(byte == 0xa5);
  sink.count = 0;
  const Record saved = records[1];
  records[1].crc ^= 1;
  assert(!WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, ignored));
  assert(sink.count == 0);
  records[1] = saved;
  --records[1].seq;
  seal(records[1]);
  assert(!WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, ignored));
  assert(sink.count == 0);
  records[1] = saved;
  records[1].boot = 1;
  seal(records[1]);
  assert(!WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, ignored));
  assert(sink.count == 0);
  records[1] = saved;
  auto invalid = batch;
  invalid.count = WaterTestUpload::batchSize + 1;
  assert(!WaterTestUpload::writeBatch(invalid, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, ignored));
  assert(sink.count == 0);
  sink.fail = true;
  assert(!WaterTestUpload::writeBatch(batch, workspace.bytes, sizeof(workspace.bytes), CountSink::write, &sink, ignored));
  printf("water_test_upload_arena: %zu record variants fit the device's 3072-byte workspace; pool %zu bytes, largest batch %zu bytes, streamed/count lengths identical, canaries intact, misalignment safe, invalid records/sequence/arena/sink failure rejected\n", cases, poolBytes, largest);
}
