#pragma once

#include "ControlContext.h"

namespace GlycolMode {

struct Context : ControlContext {
    Context(
        ControlContext& controlContext,
        GlycolLearnedParams& learnedParams,
        GlycolConfig& glycolConfig,
        GlycolRuntimeState& runtimeState,
        GlycolCooling::Algorithm requestedAlgorithm
    )
        : ControlContext(controlContext),
          learned(learnedParams),
          config(glycolConfig),
          runtime(runtimeState),
          requestedAlgorithm(requestedAlgorithm) {}

    GlycolLearnedParams& learned;
    GlycolConfig& config;
    GlycolRuntimeState& runtime;
    GlycolCooling::Algorithm requestedAlgorithm;
};

void updatePID(Context& ctx, unsigned char& integralUpdateCounter);
// Immediate fault/mode inhibition: preserves each algorithm's learned values and actual OFF time.
void suspend(Context& ctx);

// Cooling learning is RAM-only; updating state does not persist legacy parameters.
void updateState(Context& ctx);

} // namespace GlycolMode
