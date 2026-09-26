#pragma once

#include "ControlContext.h"

namespace GlycolMode {

struct Context : ControlContext {
    Context(
        ControlContext& controlContext,
        GlycolConfig& glycolConfig,
        GlycolRuntimeState& runtimeState,
        GlycolCooling::Algorithm requestedAlgorithm
    )
        : ControlContext(controlContext),
          config(glycolConfig),
          runtime(runtimeState),
          requestedAlgorithm(requestedAlgorithm) {}

    GlycolConfig& config;
    GlycolRuntimeState& runtime;
    GlycolCooling::Algorithm requestedAlgorithm;
};

void updateHeatingPID(Context& ctx, unsigned char& integralUpdateCounter);
// Immediate fault/mode inhibition: preserves each algorithm's learned values and actual OFF time.
void suspend(Context& ctx);

void updateState(Context& ctx);

} // namespace GlycolMode
