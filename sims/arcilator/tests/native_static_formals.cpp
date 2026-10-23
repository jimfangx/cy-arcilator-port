#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  for (unsigned instance = 0; instance < 2; ++instance) {
    auto model = create_model(argc, argv);
    model->initialize();
    model->advance(0, [] {});
    std::array<unsigned, 3> q{}, count{}, last{}, out{}, n{};
    auto check = [&] {
      for (unsigned i = 0; i < 3; ++i) {
        std::string index = std::to_string(i);
        for (auto [name, value] :
             std::array<std::pair<std::string, unsigned>, 6>{
                 {{"q", q[i]},
                  {"count", count[i]},
                  {"last", last[i]},
                  {"sticky", i == 2 ? 7u : 5u},
                  {"out", out[i]},
                  {"n", n[i]}}})
          if (model->signal(name + index).get() != value) {
            std::fprintf(stderr, "%s%s: got %llu expected %u\n", name.c_str(),
                         index.c_str(),
                         (unsigned long long)model->signal(name + index).get(),
                         value);
            throw std::runtime_error("static formal lifetime/copy changed");
          }
      }
    };
    check();
    for (unsigned c = 0; c < 512; ++c) {
      bool reset = c % 37 == 0, ea = c % 3 != 0, eb = c % 5 != 0,
           def = c % 7 == 0;
      unsigned data = (c * 17 + 3) & 255;
      model->advance(c * 10 + 1, [&] {
        model->signal("clock").set(0);
        model->signal("reset").set(reset);
        model->signal("enable_a").set(ea);
        model->signal("enable_b").set(eb);
        model->signal("use_default").set(def);
        model->signal("data").set(data);
      });
      check();
      for (unsigned i = 0; i < 3; ++i) {
        if (reset) {
          q[i] = count[i] = 0;
          continue;
        }
        if (!(i == 2 ? (ea ^ eb) : (i ? eb : ea)))
          continue;
        unsigned initial = i == 2 ? 7 : 5;
        unsigned a =
            def ? (i == 2 ? 11 : 9)
                : (i == 2 ? (data ^ 0xaa) : (i ? (data ^ 0x55) : data));
        if (a & (i == 2 ? 2 : 1))
          out[i] = (a + initial) & 255;
        last[i] = a ^ (i == 2 ? 0x40 : 0x80);
        n[i] = (count[i] + initial) & 255;
        count[i] = n[i];
        q[i] = out[i];
      }
      model->advance(c * 10 + 5, [&] { model->signal("clock").set(1); });
      check();
    }
  }
  std::puts("PASS: static input/output/inout state, default arguments, "
            "pre-call initialization, reset hold, two module instances, "
            "compilation-unit statics and model recreation");
}
