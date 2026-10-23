// The embedding API permits no command-line arguments. Exercise the real
// SimUART.cc/uart.cc drivers, whose initialization queries vpi_get_vlog_info.
#include "native/runtime.h"
#include "vpi_user.h"
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace chipyard_arc;
static void evaluate(void *) {}
static void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected query or uart mode");
    bool uart = std::string(argv[1]) == "uart";
    check(uart || std::string(argv[1]) == "query",
          "unknown empty-argument mode");
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "EmptyArgs", nullptr};
    Runtime runtime(0, nullptr, info, evaluate);
    if (uart) {
      register_builtin_bindings();
      auto binding = make_binding(runtime, "SimUART");
      binding->instance_name = "uart";
      unsigned offset = 24;
      for (auto name :
           {"clock", "reset", "serial_out_valid", "serial_out_ready",
            "serial_out_bits", "serial_in_valid", "serial_in_ready",
            "serial_in_bits"}) {
        unsigned bits =
            std::string(name).find("bits") != std::string::npos ? 8 : 1;
        runtime.add_signal(name, offset, bits, false);
        offset += (bits + 7) / 8;
        binding->ports[name] = runtime.signal(name);
      }
      runtime.add_binding(std::move(binding));
    }
    // UART initialization must succeed with its original VPI query. It must
    // not require a synthetic executable-name argument or altered driver.
    runtime.initialize();
    s_vpi_vlog_info args{};
    check(vpi_get_vlog_info(&args) == 1 && args.argc == 0 &&
              args.argv == nullptr && args.product && args.version,
          "empty native Runtime arguments must be a successful VPI query");
    if (uart) {
      runtime.advance_fs(0, [&] { runtime.signal("reset").set(1); });
      runtime.advance_fs(1000, [&] { runtime.signal("clock").set(1); });
      check(runtime.signal("serial_out_ready").get() == 0 &&
                runtime.signal("serial_in_valid").get() == 0,
            "original UART reset behavior changed");
      runtime.advance_fs(2000, [&] {
        runtime.signal("clock").set(0);
        runtime.signal("reset").set(0);
      });
      // Both valid and input-ready stay low: no terminal read or write.
      runtime.advance_fs(3000, [&] { runtime.signal("clock").set(1); });
      check(runtime.signal("serial_out_ready").get() == 1 &&
                runtime.signal("serial_in_valid").get() == 0,
            "original UART callback did not execute");
      runtime.advance_fs(4000, [&] { runtime.signal("clock").set(0); });
      check(runtime.signal("serial_out_ready").get() == 1,
            "UART output changed on falling edge");
    }
    std::printf("PASS: empty argv %s with original UART driver linked\n",
                argv[1]);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
