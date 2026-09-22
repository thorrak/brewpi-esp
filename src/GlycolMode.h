#pragma once

#include "ControlContext.h"

namespace GlycolMode {

struct Context : ControlContext {
    Context(
        ControlContext& controlContext,
        GlycolLearnedParams& learnedParams,
        GlycolConfig& glycolConfig,
        GlycolRuntimeState& runtimeState
    )
        : ControlContext(controlContext),
          learned(learnedParams),
          config(glycolConfig),
          runtime(runtimeState) {}

    GlycolLearnedParams& learned;
    GlycolConfig& config;
    GlycolRuntimeState& runtime;
};

void updatePID(Context& ctx, unsigned char& integralUpdateCounter);
// Immediate fault/mode inhibition: preserves learned coast and response gain and actual OFF time.
void suspend(Context& ctx);

// Always false: predictive learning is intentionally RAM-only; no legacy file writes.
bool updateState(Context& ctx);

} // namespace GlycolMode
