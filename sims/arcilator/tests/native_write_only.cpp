#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  bool previous_clock = false, previous_peer = false;
  uint64_t narrow = 0;
  std::vector<uint8_t> wide(9, 0);
  for (unsigned step = 0; step < 4096; ++step) {
    bool clock = step % 4 >= 2, peer = step % 6 >= 3;
    bool enabled = step % 7 != 0, peer_enabled = step % 11 != 0;
    std::vector<uint8_t> data(9);
    for (unsigned byte = 0; byte < 9; ++byte)
      data[byte] = uint8_t(step * 73 + byte * 37);
    data[8] &= 1;
    if (clock && !previous_clock && enabled)
      narrow = (data[0] | uint64_t(data[1]) << 8) & 8191;
    if (peer && !previous_peer && peer_enabled)
      wide = data;
    previous_clock = clock;
    previous_peer = peer;
    model->advance(step, [&] {
      model->signal("clock").set(clock);
      model->signal("peer").set(peer);
      model->signal("enable").set(enabled);
      model->signal("peer_enable").set(peer_enabled);
      model->signal("data").assign(data);
    });
    if (model->signal("narrow").get() != narrow ||
        model->signal("wide").copy() != wide)
      throw std::runtime_error("conditional write-only register mismatch");
  }
  std::puts(
      "PASS: 8192 write-only conditional register observations, 13/65-bit "
      "values, independent clocks, hold and enabled updates");
}
