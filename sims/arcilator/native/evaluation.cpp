#include "evaluation.h"

namespace {
thread_local unsigned sampling_depth = 0;
}
extern "C" uint32_t chipyard_arc_is_sampling() { return sampling_depth != 0; }
namespace chipyard_arc {
SamplingPhase::SamplingPhase() { ++sampling_depth; }
SamplingPhase::~SamplingPhase() { --sampling_depth; }
} // namespace chipyard_arc
