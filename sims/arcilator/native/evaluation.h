#pragma once
#include <cstdint>

extern "C" uint32_t chipyard_arc_is_sampling();

namespace chipyard_arc {
// Combinational DPI activations wait until all native registers have sampled
// their settled pre-edge inputs. Nested scopes restore the previous phase.
class SamplingPhase {
public:
  SamplingPhase();
  ~SamplingPhase();
  SamplingPhase(const SamplingPhase &) = delete;
  SamplingPhase &operator=(const SamplingPhase &) = delete;
};
} // namespace chipyard_arc
