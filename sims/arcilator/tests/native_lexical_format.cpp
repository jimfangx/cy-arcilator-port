#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  const char *roots[] = {"first", "second", "third"};
  const char *starts[] = {"first_start", "second_start", "third_start"};
  unsigned biases[] = {3, 3, 7};
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto model = create_model(argc, argv);
    model->signal("arc_printf_enable").set(1);
    model->initialize();
    model->advance(0, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(1);
      model->signal("enable").set(0);
      model->signal("data").set(0);
    });
    for (unsigned root = 0; root < 3; ++root) {
      auto name = roots[root];
      unsigned startup = 1 + 2 * (5 + biases[root]);
      std::fprintf(stderr,
                   "EXPECTED_HIER NATIVE_HIER_BLOCK m=[TestHarness.%s.boot]\n",
                   name);
      std::fprintf(stderr,
                   "EXPECTED_HIER NATIVE_HIER_FUNC "
                   "m=[TestHarness.%s.value.inside_function] n=5\n",
                   name);
      for (unsigned n : {7u, 9u})
        std::fprintf(stderr,
                     "EXPECTED_HIER NATIVE_HIER_FUNC "
                     "m=[TestHarness.%s.value.inside_function] n=%u\n",
                     name, n);
      std::fprintf(stderr,
                   "EXPECTED_HIER NATIVE_HIER_TASK "
                   "m=[TestHarness.%s.show.inside_task] n=%u\n",
                   name, startup);
      std::fprintf(stderr,
                   "EXPECTED_HIER NATIVE_HIER_RESULT m=[TestHarness.%s.boot] "
                   "v=%u width=32 fact=720 checks=3\n",
                   name, startup);
      if (model->signal(starts[root]).get() != startup)
        throw std::runtime_error("lexical startup function result mismatch");
    }
    unsigned expected[3] = {};
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
      bool reset = cycle % 11 == 0, enable = cycle % 3 != 1;
      unsigned data = (cycle * 29 + 5) & 255;
      model->advance(cycle * 2, [&] {
        model->signal("clock").set(0);
        model->signal("reset").set(reset);
        model->signal("enable").set(enable);
        model->signal("data").set(data);
      });
      for (unsigned root = 0; root < 3; ++root) {
        if (reset)
          expected[root] = biases[root];
        else if (enable) {
          expected[root] = (data + biases[root]) & 255;
          std::fprintf(stderr,
                       "EXPECTED_HIER NATIVE_HIER_FUNC "
                       "m=[TestHarness.%s.value.inside_function] n=%u\n",
                       roots[root], data);
          std::fprintf(stderr,
                       "EXPECTED_HIER NATIVE_HIER_TICK "
                       "m=[TestHarness.%s.tick.enabled] n=%u\n",
                       roots[root], data);
        }
      }
      model->advance(cycle * 2 + 1, [&] { model->signal("clock").set(1); });
      for (unsigned root = 0; root < 3; ++root)
        if (model->signal(roots[root]).get() != expected[root])
          throw std::runtime_error(
              "lexical clocked function register mismatch");
    }
  }
  std::fprintf(stderr,
               "PASS: lexical block/task/function scopes, transitive "
               "constant-call effects, unevaluated arguments, pure recursion "
               "and198 result checks across model recreation\n");
}
