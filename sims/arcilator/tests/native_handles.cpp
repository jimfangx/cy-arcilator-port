#include "native/runtime.h"
#include "svdpi.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
using namespace chipyard_arc;
static_assert(sizeof(void *) == sizeof(uint64_t));
struct Record {
  uint32_t value;
};
static Record records[256];
static void *expected_saved, *expected_history;
static unsigned calls, label_calls;
extern "C" void *handle_update(void *, void **, void **, const char *,
                               const char **);
extern "C" void *native_handle_identity(void *value) { return value; }
extern "C" uint32_t native_handle_value(void *value) {
  return value ? static_cast<Record *>(value)->value : 0;
}
extern "C" const char *native_handle_label(void *value, const char *prefix,
                                           void **echoed, const char **copied) {
  static char label[64];
  ++label_calls;
  *echoed = value;
  *copied = prefix;
  std::snprintf(label, sizeof(label), "%s:%u", prefix,
                native_handle_value(value));
  return label;
}
extern "C" void *native_handle_step(void *prior, uint32_t key, void **produced,
                                    void **history) {
  if (prior != expected_saved || *history != expected_history)
    throw std::runtime_error("native opaque pointer input/inout mismatch");
  ++calls;
  *produced = key % 7 ? &records[key ^ 0x5a] : nullptr;
  *history = key % 11 ? &records[key ^ 0xa5] : nullptr;
  return key % 5 ? &records[key] : nullptr;
}
int main(int argc, char **argv) {
  if (uintptr_t(records) <= UINT32_MAX)
    throw std::runtime_error("fixture must exercise pointers above 32 bits");
  for (unsigned instance = 0; instance < 2; ++instance) {
    calls = label_calls = 0;
    expected_saved = expected_history = nullptr;
    for (unsigned i = 0; i < 256; ++i)
      records[i].value = 0x12345678u + i * 0x1020304u;
    auto m = create_model(argc, argv);
    m->initialize();
    auto scope = svGetScopeFromName("HandlesHarness/dut/rtl/rtl");
    if (!scope)
      throw std::runtime_error("native handle export scope missing");
    svSetScope(scope);
    bool rejected = false;
    try {
      void *selected = nullptr;
      const char *previous_note = nullptr;
      handle_update(nullptr, nullptr, &selected, "", &previous_note);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error("null output reference was accepted");
    std::string expected_note;
    uint32_t values = 0, flags = 0;
    unsigned expected_calls = 0;
    for (unsigned cycle = 0; cycle < 256; ++cycle) {
      bool reset = cycle % 31 == 0, enable = cycle % 6 != 0;
      m->advance(cycle * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
        m->signal("key").set(cycle);
      });
      m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
      if (reset) {
        expected_saved = expected_history = nullptr;
        values = flags = 0;
        expected_note.clear();
      } else if (enable) {
        ++expected_calls;
        void *returned = cycle % 5 ? &records[cycle] : nullptr;
        void *produced = cycle % 7 ? &records[cycle ^ 0x5a] : nullptr;
        expected_history = cycle % 11 ? &records[cycle ^ 0xa5] : nullptr;
        expected_saved = returned;
        values = native_handle_value(returned) ^
                 (native_handle_value(produced) << 1) ^
                 (native_handle_value(expected_history) << 2);
        std::string prefix = cycle & 2 ? "ptr" : "";
        values ^= (prefix + ":" + std::to_string(native_handle_value(returned)))
                      .size();
        values ^= uint32_t(prefix.size()) << 24;
        flags = (returned == nullptr) | ((produced != nullptr) << 1) |
                ((expected_history == produced) << 2) |
                ((returned != expected_history) << 3) | (1u << 4);
      }
      if (m->signal("values").get() != values ||
          m->signal("flags").get() != flags || calls != expected_calls ||
          label_calls != expected_calls)
        throw std::runtime_error(
            "opaque pointer return/output, equality or clock gating mismatch");
      if (cycle % 9 == 4) {
        void *value = cycle % 3 ? &records[(cycle + 31) % 256] : nullptr;
        void *previous = &records[255];
        void *selected = cycle % 2 ? &records[(cycle + 63) % 256] : nullptr;
        void *wanted = selected ? selected : expected_history;
        std::string text = "note" + std::to_string(cycle);
        const char *previous_note = nullptr;
        void *result = handle_update(value, &previous, &selected, text.c_str(),
                                     &previous_note);
        if (result != value || previous != expected_saved ||
            selected != wanted || !previous_note ||
            expected_note != previous_note)
          throw std::runtime_error(
              "opaque pointer exported C ABI/captured state mismatch");
        expected_saved = value;
        expected_history = wanted;
        expected_note = text;
      }
    }
  }
  std::puts(
      "PASS: 1024 opaque pointer observations plus C/SV exported handle "
      "updates, "
      "64-bit addresses, aliases, pure identity, null/equality, output/inout, "
      "captured storage, mixed strings, reset/enable and model recreation");
}
