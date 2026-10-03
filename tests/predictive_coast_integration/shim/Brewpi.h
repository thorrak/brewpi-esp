#pragma once
// Host platform boundary only. Algorithm sources are unmodified in ../vendor.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define TEMP_CONTROL_STATIC 1
#define OPTIMIZE_GLOBAL 0
#define logDebug(...) ((void)0)
namespace Config { namespace TempFormat {
constexpr int bufferLen = 12;
constexpr int tempDecimals = 1;
constexpr int tempDiffDecimals = 3;
constexpr int fixedPointDecimals = 3;
} }
