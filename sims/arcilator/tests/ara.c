#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum { N = 257 };
static uint32_t a[N] __attribute__((aligned(64)));
static uint32_t b[N] __attribute__((aligned(64)));
static uint32_t c[N] __attribute__((aligned(64)));
static float fa[N] __attribute__((aligned(64)));
static float fb[N] __attribute__((aligned(64)));
static float fc[N] __attribute__((aligned(64)));

int main(void) {
  // Enable the vector and floating-point contexts as in Ara's crt0.S.
  uintptr_t enabled = 0x2200;
  asm volatile("csrs mstatus, %0" :: "r"(enabled) : "memory");
  for (unsigned i = 0; i < N; ++i) {
    a[i] = UINT32_C(0x9e3779b9) * i + UINT32_C(0xf1234567);
    b[i] = UINT32_C(0x31415927) * i + UINT32_C(0x87654321);
    fa[i] = (float)i * 0.5f;
    fb[i] = (float)i * 0.25f;
  }
  for (size_t i = 0; i < N;) {
    size_t vl;
    asm volatile("vsetvli %0, %1, e32, m1, ta, ma" : "=r"(vl) : "r"(N-i));
    if (!vl || vl > N-i) return 1;
    asm volatile("vle32.v v8, (%0)\n\t"
                 "vle32.v v16, (%1)\n\t"
                 "vadd.vv v24, v8, v16\n\t"
                 "vse32.v v24, (%2)"
                 :: "r"(a+i), "r"(b+i), "r"(c+i)
                 : "v8", "v16", "v24", "memory");
    asm volatile("vle32.v v8, (%0)\n\t"
                 "vle32.v v16, (%1)\n\t"
                 "vfadd.vv v24, v8, v16\n\t"
                 "vse32.v v24, (%2)"
                 :: "r"(fa+i), "r"(fb+i), "r"(fc+i)
                 : "v8", "v16", "v24", "memory");
    i += vl;
  }
  for (unsigned i = 0; i < N; ++i) {
    if (c[i] != a[i] + b[i]) {
      printf("FAIL: Ara integer lane %u\n", i);
      return 2;
    }
    if (fc[i] != (float)i * 0.75f) {
      printf("FAIL: Ara floating-point lane %u\n", i);
      return 3;
    }
  }
  printf("PASS: Ara 257 integer and 257 floating-point vector elements\n");
  return 0;
}
