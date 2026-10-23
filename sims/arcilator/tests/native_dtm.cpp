#include "native/runtime.h"
#include <cstdio>

static unsigned calls = 0;
static std::string mode;
extern "C" int debug_tick(unsigned char *valid, unsigned char request_ready,
                          int *address, int *operation, int *data,
                          unsigned char response_valid,
                          unsigned char *response_ready, int response,
                          int response_data) {
  ++calls;
  if (!mode.empty()) {
    bool arrived = mode == "fractional-input" && calls == 2;
    if (request_ready != arrived || response_valid != arrived ||
        response != (arrived ? 3 : 0) ||
        uint32_t(response_data) != (arrived ? 0xfedcba98u : 0))
      throw std::runtime_error(
          "SimDTM fractional input arrived before its deadline");
    *valid = *response_ready = 1;
    *address = 0xfe;
    *operation = 7;
    *data = 0x11223344;
    return 0x12340000 | calls;
  }
  bool arrived = calls == 2;
  if (calls > 2 || request_ready != arrived || response_valid != arrived ||
      response != (arrived ? 3 : 0) ||
      uint32_t(response_data) != (arrived ? 0xfedcba98u : 0))
    throw std::runtime_error(
        "SimDTM did not sample the original delayed inputs");
  *valid = *response_ready = !arrived;
  *address = arrived ? 0x85 : 0xfe;
  *operation = arrived ? 4 : 7;
  *data = arrived ? 0x88776655 : 0x11223344;
  return 0x12340000 | calls;
}
static void standard(int argc, char **argv) {
  calls = 0;
  auto r = chipyard_arc::create_model(argc, argv);
  r->initialize();
  auto value = [&](const char *name, uint64_t expected) {
    if (r->signal(name).get() != expected)
      throw std::runtime_error(std::string("SimDTM output mismatch: ") + name);
  };
  r->advance(0, [&] { r->signal("reset").set(1); });
  r->advance(500, [&] { r->signal("clock").set(1); });
  r->advance(750, [&] {
    r->signal("clock").set(0);
    r->signal("reset").set(0);
  });
  r->advance(1000, [&] { r->signal("clock").set(1); });
  if (calls)
    throw std::runtime_error("SimDTM lost its registered-reset guard");
  r->advance(1250, [&] { r->signal("clock").set(0); });
  r->advance(1450, [&] {
    r->signal("request_ready").set(1);
    r->signal("response_valid").set(1);
    r->signal("response").set(3);
    r->signal("response_data").set(0xfedcba98);
  });
  r->advance(1500, [&] { r->signal("clock").set(1); });
  if (calls != 1)
    throw std::runtime_error("SimDTM used the wrong clock edge");
  r->advance(1599);
  value("request_valid", 0);
  value("exit", 0);
  r->advance(1600);
  value("request_valid", 1);
  value("response_ready", 1);
  value("address", 0x7e);
  value("operation", 3);
  value("data", 0x11223344);
  value("exit", 0x12340001);
  r->advance(1750, [&] { r->signal("clock").set(0); });
  r->advance(2000, [&] { r->signal("clock").set(1); });
  r->advance(2099);
  value("request_valid", 1);
  value("exit", 0x12340001);
  r->advance(2100);
  value("request_valid", 0);
  value("response_ready", 0);
  value("address", 5);
  value("operation", 0);
  value("data", 0x88776655);
  value("exit", 0x12340002);
  r->advance(2150, [&] { r->signal("reset").set(1); });
  r->advance(2250, [&] { r->signal("clock").set(0); });
  r->advance(2500, [&] { r->signal("clock").set(1); });
  r->advance(2599);
  value("exit", 0x12340002);
  r->advance(2600);
  value("exit", 0);
  value("address", 5);
  value("data", 0x88776655);
  if (calls != 2)
    throw std::runtime_error("SimDTM called its driver during reset");
  std::puts("PASS: legacy SimDTM clock edge, delayed inputs/outputs, width "
            "masking, and registered reset");
}

static void fractional(int argc, char **argv) {
  calls = 0;
  auto r = chipyard_arc::create_model(argc, argv);
  r->initialize();
  auto value = [&](const char *name, uint64_t expected) {
    if (r->signal(name).get() != expected)
      throw std::runtime_error(
          std::string("SimDTM fractional output mismatch: ") + name);
  };
  r->advance_fs(0, [&] { r->signal("reset").set(1); });
  r->advance_fs(1000, [&] { r->signal("clock").set(1); });
  r->advance_fs(3000, [&] {
    r->signal("clock").set(0);
    r->signal("reset").set(0);
  });
  r->advance_fs(5000, [&] { r->signal("clock").set(1); });
  r->advance_fs(6000, [&] { r->signal("clock").set(0); });
  if (calls)
    throw std::runtime_error("SimDTM fractional test lost registered reset");
  if (mode == "fractional-output") {
    r->advance_fs(9500, [&] { r->signal("clock").set(1); });
    if (calls != 1)
      throw std::runtime_error(
          "SimDTM fractional test did not call original driver ABI");
    r->advance_fs(109250);
    for (auto name : {"request_valid", "address", "operation", "data",
                      "response_ready", "exit"})
      value(name, 0);
    r->advance_fs(109500);
    value("request_valid", 1);
    value("response_ready", 1);
    value("address", 0x7e);
    value("operation", 3);
    value("data", 0x11223344);
    value("exit", 0x12340001);
  } else {
    r->advance_fs(12500, [&] {
      r->signal("request_ready").set(1);
      r->signal("response_valid").set(1);
      r->signal("response").set(3);
      r->signal("response_data").set(0xfedcba98);
    });
    r->advance_fs(112250, [&] { r->signal("clock").set(1); });
    r->advance_fs(112375, [&] { r->signal("clock").set(0); });
    r->advance_fs(113000, [&] { r->signal("clock").set(1); });
    if (calls != 2)
      throw std::runtime_error(
          "SimDTM fractional input callback count changed");
  }
  std::printf("PASS: legacy SimDTM %s\n", mode.c_str());
}

int main(int argc, char **argv) {
  try {
    if (argc == 1 || std::string(argv[1]) == "standard")
      standard(argc, argv);
    for (auto name : {"fractional-output", "fractional-input"}) {
      if (argc > 1 && std::string(argv[1]) != name)
        continue;
      mode = name;
      for (unsigned recreation = 0; recreation < 2; ++recreation)
        fractional(argc, argv);
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
