#pragma once
#include <cstddef>

namespace WaterTestStorage {
constexpr const char *journalPath = "/water-test-records.bin";
size_t freeBytes();
bool removePreviousDataset();
} // namespace WaterTestStorage
