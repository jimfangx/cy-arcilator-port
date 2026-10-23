// Exercise the real DPI driver on empty, waiting, and completed ROB entries.
#include <array>
#include <cassert>
#include <cstring>

extern "C" void debug_rob_push_trace(int, char, char, long long, char, long long,
                                    long long, int, char, char, long long, long long, long long *);
extern "C" void debug_rob_push_wb(int, char, long long, long long *);
extern "C" void debug_rob_pop_trace(int, char *, long long *, long long *, int *,
                                   char *, char *, long long *, long long *, long long *);

struct Trace {
  char valid, exception, interrupt;
  int priv;
  long long pc, insn, cause, tval;
  std::array<long long, 8> data;
};

static Trace pop(int hart) {
  Trace t;
  std::memset(&t, 0x5a, sizeof(t));
  debug_rob_pop_trace(hart, &t.valid, &t.pc, &t.insn, &t.priv, &t.exception,
                      &t.interrupt, &t.cause, &t.tval, t.data.data());
  return t;
}

static void empty(const Trace &t) {
  assert(!t.valid && !t.exception && !t.interrupt && !t.priv);
  assert(!t.pc && !t.insn && !t.cause && !t.tval);
  for (auto word : t.data) assert(word == 0);
}

int main() {
  empty(pop(0));
  std::array<long long, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
  debug_rob_push_trace(0, 1, 0, 77, 1, 0x80000000, 0x13, 3, 0, 0, 0, 0, data.data());
  empty(pop(0)); // The instruction is still waiting for its writeback.
  for (auto &word : data) word += 10;
  debug_rob_push_wb(0, 1, 77, data.data());
  Trace t = pop(0);
  assert(t.valid && t.pc == 0x80000000 && t.insn == 0x13 && t.priv == 3);
  assert(!t.exception && !t.interrupt && t.data == data);
  empty(pop(0));
  debug_rob_push_trace(1, 0, 1, 0, 1, 0x10000, 0x73, 3, 1, 0, 2, 0x1234, data.data());
  t = pop(1);
  assert(t.valid && t.exception && !t.interrupt && t.cause == 2 && t.tval == 0x1234);
  assert(t.data == data);
  empty(pop(1));
}
