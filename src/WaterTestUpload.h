#pragma once
#include "WaterTestProtocol.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

// The same serializer is used for Content-Length, transmission, and native
// portal contract tests. A batch never needs a complete JSON document or body.
namespace WaterTestUpload {
constexpr size_t batchSize = 12;
constexpr size_t workspaceBytes = ARDUINOJSON_SIZEOF_POINTER > 4 ? 8192 : 3072;
using Sink = bool (*)(void *, const char *, size_t);
struct Batch {
  const char *deviceGuid, *testId, *batchId, *bootId;
  const WaterTestCore::Record *records;
  size_t count;
  double beerOffset, glycolOffset;
};

namespace Detail {
// Reset only after the document is destroyed. There is deliberately no heap
// fallback: a future record schema that exceeds the bound must fail explicitly.
class Arena final : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Header { size_t size; };
  unsigned char *begin_;
  size_t capacity_, used_ = 0;
public:
  Arena(void *memory, size_t capacity) : begin_(static_cast<unsigned char *>(memory)), capacity_(capacity) {}
  void reset() { used_ = 0; }
  void *allocate(size_t size) override {
    constexpr size_t alignment = alignof(Header);
    const uintptr_t address = reinterpret_cast<uintptr_t>(begin_) + used_;
    const size_t padding = (alignment - address % alignment) % alignment;
    if (used_ > capacity_ || padding > capacity_ - used_ ||
        sizeof(Header) > capacity_ - used_ - padding ||
        size > capacity_ - used_ - padding - sizeof(Header))
      return nullptr;
    auto *header = new (begin_ + used_ + padding) Header{size};
    used_ += padding + sizeof(Header) + size;
    return header + 1;
  }
  void deallocate(void *) override {}
  void *reallocate(void *old, size_t size) override {
    if (!old)
      return allocate(size);
    const size_t previous = (static_cast<Header *>(old) - 1)->size;
    if (size <= previous)
      return old;
    void *replacement = allocate(size);
    if (replacement)
      std::memcpy(replacement, old, previous);
    return replacement;
  }
};

class Writer {
  Sink sink_;
  void *context_;
  size_t remaining_ = std::numeric_limits<size_t>::max();
  size_t length_ = 0;
  bool ok_ = true;
public:
  Writer(Sink sink, void *context) : sink_(sink), context_(context) {}
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t write(const uint8_t *bytes, size_t size) {
    if (!ok_)
      return 0;
    const size_t emitted = std::min(size, remaining_);
    if (emitted && sink_ && !sink_(context_, reinterpret_cast<const char *>(bytes), emitted)) {
      ok_ = false;
      return 0;
    }
    remaining_ -= emitted;
    length_ += emitted;
    // A limited write intentionally omits the header's final closing brace.
    return size;
  }
  bool text(const char *value) {
    const size_t size = std::strlen(value);
    return write(reinterpret_cast<const uint8_t *>(value), size) == size;
  }
  void limit(size_t count = std::numeric_limits<size_t>::max()) { remaining_ = count; }
  bool ok() const { return ok_; }
  size_t length() const { return length_; }
};
} // namespace Detail

// A null sink performs an identical dry run. Caller owns the immutable snapshot
// and workspace until transmission ends; both passes produce exactly the same JSON.
inline bool writeBatch(const Batch &batch, void *workspace, size_t capacity, Sink sink,
                       void *sinkContext, size_t &length) {
  length = 0;
  if (!workspace || !batch.records || !batch.count || batch.count > batchSize ||
      !batch.deviceGuid || !batch.testId || !batch.batchId || !batch.bootId)
    return false;
  for (size_t i = 0; i < batch.count; ++i)
    if (!WaterTestCore::valid(batch.records[i]) || batch.records[i].boot != 0 ||
        (i && (batch.records[i - 1].seq == UINT32_MAX || batch.records[i].seq != batch.records[i - 1].seq + 1)))
      return false;
  Detail::Arena arena(workspace, capacity);
  Detail::Writer writer(sink, sinkContext);
  {
    JsonDocument header(&arena);
    header["schema_version"] = 1;
    header["device_guid"] = batch.deviceGuid;
    header["test_id"] = batch.testId;
    header["first_seq"] = batch.records[0].seq;
    header["last_seq"] = batch.records[batch.count - 1].seq;
    header["batch_id"] = batch.batchId;
    header["boot_id"] = batch.bootId;
    if (header.overflowed())
      return false;
    const size_t expected = measureJson(header);
    writer.limit(expected - 1);
    if (serializeJson(header, writer) != expected || !writer.ok())
      return false;
    writer.limit();
  }
  if (!writer.text(",\"records\":["))
    return false;
  for (size_t i = 0; i < batch.count; ++i) {
    arena.reset();
    JsonDocument record(&arena);
    const auto &source = batch.records[i];
    WaterTestProtocol::recordToJson(record.to<JsonObject>(), source, batch.bootId,
                                   source.role ? batch.glycolOffset : batch.beerOffset);
    if (record.overflowed() || (i && !writer.text(",")))
      return false;
    if (serializeJson(record, writer) != measureJson(record) || !writer.ok())
      return false;
  }
  if (!writer.text("]}"))
    return false;
  length = writer.length();
  return true;
}
} // namespace WaterTestUpload
