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
// Returns true when learned glycol parameters should be persisted by the caller.
bool updateState(Context& ctx);

} // namespace GlycolMode
