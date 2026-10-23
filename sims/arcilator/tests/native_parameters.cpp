#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  model->advance(0);
  if (!model->signal("ok").get())
    throw std::runtime_error(
        "native SV parameter specialization changed values");
  model->advance(17);
  if (!model->signal("ok").get())
    throw std::runtime_error("native constant parameter result changed");
  std::puts("PASS: native SV boolean, signed integer, real, escaped and UTF-8 "
            "parameter specialization");
}
