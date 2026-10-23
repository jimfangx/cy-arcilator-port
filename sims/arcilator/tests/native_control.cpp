#include "native/runtime.h"

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->signal("fail").set(runtime->flag("test-failure"));
  return runtime->run();
}
