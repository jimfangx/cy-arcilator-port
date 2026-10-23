#include "native/runtime.h"
#include <array>
#include <cstdio>
using namespace chipyard_arc;
static unsigned bits(const std::vector<uint8_t> &p, unsigned start,
                     unsigned width) {
  unsigned r = 0;
  for (unsigned i = 0; i < width; i++)
    r |= unsigned((p[(start + i) / 8] >> ((start + i) % 8)) & 1) << i;
  return r;
}
int main(int argc, char **argv) {
  auto m = create_model(argc, argv);
  m->initialize();
  unsigned checks = 0;
  for (unsigned n = 0; n < 4096; n++) {
    unsigned active = n & 511, banks = 0;
    std::vector<uint8_t> expected(9);
    for (unsigned q = 0; q < 9; q++) {
      unsigned bank = (n * 73 + q * 3) % 8;
      banks |= bank << (q * 3);
      if (active & (1u << q))
        expected[(bank * 9 + q) / 8] |= 1 << ((bank * 9 + q) % 8);
    }
    unsigned ti = (n / 4) % 8, pi = (n / 7) % 8, ri = n % 8, tv = (n * 13) & 31,
             pv = (n * 61) & 8191, we = (n / 11) % 4;
    std::array<unsigned, 5> tags{}, payloads{};
    if ((we & 1) && ti < 5)
      tags[ti] = tv;
    if ((we & 2) && pi < 5)
      payloads[pi] = pv;
    m->advance(n, [&] {
      m->signal("active").set(active);
      m->signal("banks").set(banks);
      m->signal("tag_index").set(ti);
      m->signal("payload_index").set(pi);
      m->signal("read_index").set(ri);
      m->signal("tag_value").set(tv);
      m->signal("payload_value").set(pv);
      m->signal("write_enable").set(we);
    });
    auto actual = m->signal("matrix").copy();
    if (actual != expected) {
      std::fprintf(stderr, "matrix n=%u active=%x banks=%x\n", n, active,
                   banks);
      throw std::runtime_error("dynamic matrix update mismatch");
    }
    checks += 72;
    auto fields = m->signal("fields").copy();
    for (unsigned i = 0; i < 5; i++) {
      if (bits(fields, i * 18 + 13, 5) != tags[i] ||
          bits(fields, i * 18, 13) != payloads[i]) {
        std::fprintf(stderr, "fields n=%u i=%u\n", n, i);
        throw std::runtime_error("dynamic struct field update mismatch");
      }
      checks += 2;
    }
    if (m->signal("read_tag").get() != (ri < 5 ? tags[ri] : 0) ||
        m->signal("read_payload").get() != (ri < 5 ? payloads[ri] : 0))
      throw std::runtime_error("dynamic selected field mismatch");
    checks += 2;
    unsigned chained = tags[0] & 7;
    if (m->signal("chained_payload").get() !=
        (chained < 5 ? payloads[chained] : 0))
      throw std::runtime_error("probe-dependent dynamic index mismatch");
    ++checks;
  }
  std::printf("PASS: %u dynamic signal write comparisons\n", checks);
}
