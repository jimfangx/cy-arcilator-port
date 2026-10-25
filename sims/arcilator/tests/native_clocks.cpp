#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      throw std::runtime_error("expected mode, oracle and trace files");
    bool override = std::string(argv[1]) == "override";
    if (!override && std::string(argv[1]) != "default")
      throw std::runtime_error("unknown original clock mode");
    char name[] = "original-clocks", frequency[] = "+UNIT_CLOCK_MHZ=125";
    char *args[] = {name, frequency};
    auto runtime = create_model(override ? 2 : 1, args);
    runtime->signal("power").set(1);
    runtime->signal("gate").set(0);
    runtime->signal("reset").set(0);
    runtime->initialize();
    std::ifstream oracle(argv[2]);
    FILE *file = std::fopen(argv[3], "wb");
    if (!oracle || !file)
      throw std::runtime_error("cannot open owned original clock oracle/trace");
    uint64_t time;
    unsigned power, gate, drive;
    std::array<unsigned, 6> expected;
    constexpr std::array<const char *, 6> names{{"fixed", "harness", "plusarg",
                                                 "fixed_count", "harness_count",
                                                 "plusarg_count"}};
    unsigned rows = 0;
    while (oracle >> time >> power >> gate >> drive >> expected[0] >>
           expected[1] >> expected[2] >> expected[3] >> expected[4] >>
           expected[5]) {
      unsigned callbacks = 0;
      runtime->advance_fs(time, [&] {
        ++callbacks;
        if (drive) {
          runtime->signal("power").set(power);
          runtime->signal("gate").set(gate);
        }
      });
      if (callbacks != 1 || runtime->time_fs() != time)
        throw std::runtime_error(
            "original clock external drive/time contract changed");
      for (unsigned i = 0; i < names.size(); ++i) {
        auto value = runtime->signal(names[i]).get();
        if (value != expected[i]) {
          std::fprintf(stderr,
                       "clock mismatch mode=%s time_fs=%llu field=%s "
                       "actual=%llu expected=%u\n",
                       argv[1], (unsigned long long)time, names[i],
                       (unsigned long long)value, expected[i]);
          throw std::runtime_error("original clock levels/counters mismatch");
        }
      }
      for (unsigned i = 0; i < 8; ++i)
        if (std::fputc(uint8_t(time >> (i * 8)), file) == EOF)
          throw std::runtime_error("original clock trace write failed");
      for (const auto *field : names) {
        auto value = runtime->signal(field).copy();
        if (std::fwrite(value.data(), value.size(), 1, file) != 1)
          throw std::runtime_error("original clock trace value write failed");
      }
      ++rows;
    }
    if (!oracle.eof() || !rows || std::fclose(file))
      throw std::runtime_error("original clock oracle/trace completion failed");
    std::printf("PASS original clocks: mode=%s samples=%u\n", argv[1], rows);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
