#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <riscv_vector.h>
#define N 37
static const double x[N] __attribute__((aligned(64))) = {
  -0x1.4000000000000p+2,
  -0x1.3000000000000p+2,
  -0x1.2000000000000p+2,
  -0x1.1000000000000p+2,
  -0x1.0000000000000p+2,
  -0x1.e000000000000p+1,
  -0x1.c000000000000p+1,
  -0x1.a000000000000p+1,
  -0x1.8000000000000p+1,
  -0x1.6000000000000p+1,
  -0x1.4000000000000p+1,
  -0x1.2000000000000p+1,
  -0x1.0000000000000p+1,
  -0x1.c000000000000p+0,
  -0x1.8000000000000p+0,
  -0x1.4000000000000p+0,
  -0x1.0000000000000p+0,
  -0x1.8000000000000p-1,
  -0x1.0000000000000p-1,
  -0x1.0000000000000p-2,
  0x0.0p+0,
  0x1.0000000000000p-2,
  0x1.0000000000000p-1,
  0x1.8000000000000p-1,
  0x1.0000000000000p+0,
  0x1.4000000000000p+0,
  0x1.8000000000000p+0,
  0x1.c000000000000p+0,
  0x1.0000000000000p+1,
  0x1.2000000000000p+1,
  0x1.4000000000000p+1,
  0x1.6000000000000p+1,
  0x1.8000000000000p+1,
  0x1.a000000000000p+1,
  0x1.c000000000000p+1,
  0x1.e000000000000p+1,
  0x1.0000000000000p+2
};
static const double y[N] __attribute__((aligned(64))) = {
  -0x1.e000000000000p+2,
  -0x1.8000000000000p+2,
  -0x1.2000000000000p+2,
  -0x1.8000000000000p+1,
  -0x1.8000000000000p+0,
  0x0.0p+0,
  0x1.8000000000000p+0,
  0x1.8000000000000p+1,
  0x1.2000000000000p+2,
  0x1.8000000000000p+2,
  0x1.e000000000000p+2,
  0x1.2000000000000p+3,
  0x1.5000000000000p+3,
  0x1.8000000000000p+3,
  0x1.b000000000000p+3,
  0x1.e000000000000p+3,
  0x1.0800000000000p+4,
  0x1.2000000000000p+4,
  0x1.3800000000000p+4,
  0x1.5000000000000p+4,
  0x1.6800000000000p+4,
  0x1.8000000000000p+4,
  0x1.9800000000000p+4,
  0x1.b000000000000p+4,
  0x1.c800000000000p+4,
  0x1.e000000000000p+4,
  0x1.f800000000000p+4,
  0x1.0800000000000p+5,
  0x1.1400000000000p+5,
  0x1.2000000000000p+5,
  0x1.2c00000000000p+5,
  0x1.3800000000000p+5,
  0x1.4400000000000p+5,
  0x1.5000000000000p+5,
  0x1.5c00000000000p+5,
  0x1.6800000000000p+5,
  0x1.7400000000000p+5
};
static const uint64_t expected_fp[N] __attribute__((aligned(64))) = {
  UINT64_C(0xc024000000000000),
  UINT64_C(0xc020c00000000000),
  UINT64_C(0xc01b000000000000),
  UINT64_C(0xc014800000000000),
  UINT64_C(0xc00c000000000000),
  UINT64_C(0xbffe000000000000),
  UINT64_C(0xbfd0000000000000),
  UINT64_C(0x3ff6000000000000),
  UINT64_C(0x4008000000000000),
  UINT64_C(0x4012800000000000),
  UINT64_C(0x4019000000000000),
  UINT64_C(0x401f800000000000),
  UINT64_C(0x4023000000000000),
  UINT64_C(0x4026400000000000),
  UINT64_C(0x4029800000000000),
  UINT64_C(0x402cc00000000000),
  UINT64_C(0x4030000000000000),
  UINT64_C(0x4031a00000000000),
  UINT64_C(0x4033400000000000),
  UINT64_C(0x4034e00000000000),
  UINT64_C(0x4036800000000000),
  UINT64_C(0x4038200000000000),
  UINT64_C(0x4039c00000000000),
  UINT64_C(0x403b600000000000),
  UINT64_C(0x403d000000000000),
  UINT64_C(0x403ea00000000000),
  UINT64_C(0x4040200000000000),
  UINT64_C(0x4040f00000000000),
  UINT64_C(0x4041c00000000000),
  UINT64_C(0x4042900000000000),
  UINT64_C(0x4043600000000000),
  UINT64_C(0x4044300000000000),
  UINT64_C(0x4045000000000000),
  UINT64_C(0x4045d00000000000),
  UINT64_C(0x4046a00000000000),
  UINT64_C(0x4047700000000000),
  UINT64_C(0x4048400000000000)
};
static const uint64_t a[N] __attribute__((aligned(64))) = {
  UINT64_C(0xfedcba9876543210),
  UINT64_C(0xffffffffffffffff),
  UINT64_C(0x0123456789abcdee),
  UINT64_C(0x02468acf13579bdd),
  UINT64_C(0x0369d0369d0369cc),
  UINT64_C(0x048d159e26af37bb),
  UINT64_C(0x05b05b05b05b05aa),
  UINT64_C(0x06d3a06d3a06d399),
  UINT64_C(0x07f6e5d4c3b2a188),
  UINT64_C(0x091a2b3c4d5e6f77),
  UINT64_C(0x0a3d70a3d70a3d66),
  UINT64_C(0x0b60b60b60b60b55),
  UINT64_C(0x0c83fb72ea61d944),
  UINT64_C(0x0da740da740da733),
  UINT64_C(0x0eca8641fdb97522),
  UINT64_C(0x0fedcba987654311),
  UINT64_C(0x1111111111111100),
  UINT64_C(0x123456789abcdeef),
  UINT64_C(0x13579be02468acde),
  UINT64_C(0x147ae147ae147acd),
  UINT64_C(0x159e26af37c048bc),
  UINT64_C(0x16c16c16c16c16ab),
  UINT64_C(0x17e4b17e4b17e49a),
  UINT64_C(0x1907f6e5d4c3b289),
  UINT64_C(0x1a2b3c4d5e6f8078),
  UINT64_C(0x1b4e81b4e81b4e67),
  UINT64_C(0x1c71c71c71c71c56),
  UINT64_C(0x1d950c83fb72ea45),
  UINT64_C(0x1eb851eb851eb834),
  UINT64_C(0x1fdb97530eca8623),
  UINT64_C(0x20fedcba98765412),
  UINT64_C(0x2222222222222201),
  UINT64_C(0x23456789abcdeff0),
  UINT64_C(0x2468acf13579bddf),
  UINT64_C(0x258bf258bf258bce),
  UINT64_C(0x26af37c048d159bd),
  UINT64_C(0x27d27d27d27d27ac)
};
static const uint64_t b[N] __attribute__((aligned(64))) = {
  UINT64_C(0x8000000000000001),
  UINT64_C(0xf17539ab7c635dfe),
  UINT64_C(0x62ea7356f8c6bbff),
  UINT64_C(0xd45fad02752a19fc),
  UINT64_C(0x45d4e6adf18d77fd),
  UINT64_C(0xb74a20596df0d5fa),
  UINT64_C(0x28bf5a04ea5433fb),
  UINT64_C(0x9a3493b066b791f8),
  UINT64_C(0x0ba9cd5be31aeff9),
  UINT64_C(0x7d1f07075f7e4df6),
  UINT64_C(0xee9440b2dbe1abf7),
  UINT64_C(0x60097a5e584509f4),
  UINT64_C(0xd17eb409d4a867f5),
  UINT64_C(0x42f3edb5510bc5f2),
  UINT64_C(0xb4692760cd6f23f3),
  UINT64_C(0x25de610c49d281f0),
  UINT64_C(0x97539ab7c635dff1),
  UINT64_C(0x08c8d46342993dee),
  UINT64_C(0x7a3e0e0ebefc9bef),
  UINT64_C(0xebb347ba3b5ff9ec),
  UINT64_C(0x5d288165b7c357ed),
  UINT64_C(0xce9dbb113426b5ea),
  UINT64_C(0x4012f4bcb08a13eb),
  UINT64_C(0xb1882e682ced71e8),
  UINT64_C(0x22fd6813a950cfe9),
  UINT64_C(0x9472a1bf25b42de6),
  UINT64_C(0x05e7db6aa2178be7),
  UINT64_C(0x775d15161e7ae9e4),
  UINT64_C(0xe8d24ec19ade47e5),
  UINT64_C(0x5a47886d1741a5e2),
  UINT64_C(0xcbbcc21893a503e3),
  UINT64_C(0x3d31fbc4100861e0),
  UINT64_C(0xaea7356f8c6bbfe1),
  UINT64_C(0x201c6f1b08cf1dde),
  UINT64_C(0x9191a8c685327bdf),
  UINT64_C(0x0306e2720195d9dc),
  UINT64_C(0x747c1c1d7df937dd)
};
static const uint64_t expected_add[N] __attribute__((aligned(64))) = {
  UINT64_C(0x7edcba9876543211),
  UINT64_C(0xf17539ab7c635dfd),
  UINT64_C(0x640db8be827289ed),
  UINT64_C(0xd6a637d18881b5d9),
  UINT64_C(0x493eb6e48e90e1c9),
  UINT64_C(0xbbd735f794a00db5),
  UINT64_C(0x2e6fb50a9aaf39a5),
  UINT64_C(0xa108341da0be6591),
  UINT64_C(0x13a0b330a6cd9181),
  UINT64_C(0x86393243acdcbd6d),
  UINT64_C(0xf8d1b156b2ebe95d),
  UINT64_C(0x6b6a3069b8fb1549),
  UINT64_C(0xde02af7cbf0a4139),
  UINT64_C(0x509b2e8fc5196d25),
  UINT64_C(0xc333ada2cb289915),
  UINT64_C(0x35cc2cb5d137c501),
  UINT64_C(0xa864abc8d746f0f1),
  UINT64_C(0x1afd2adbdd561cdd),
  UINT64_C(0x8d95a9eee36548cd),
  UINT64_C(0x002e2901e97474b9),
  UINT64_C(0x72c6a814ef83a0a9),
  UINT64_C(0xe55f2727f592cc95),
  UINT64_C(0x57f7a63afba1f885),
  UINT64_C(0xca90254e01b12471),
  UINT64_C(0x3d28a46107c05061),
  UINT64_C(0xafc123740dcf7c4d),
  UINT64_C(0x2259a28713dea83d),
  UINT64_C(0x94f2219a19edd429),
  UINT64_C(0x078aa0ad1ffd0019),
  UINT64_C(0x7a231fc0260c2c05),
  UINT64_C(0xecbb9ed32c1b57f5),
  UINT64_C(0x5f541de6322a83e1),
  UINT64_C(0xd1ec9cf93839afd1),
  UINT64_C(0x44851c0c3e48dbbd),
  UINT64_C(0xb71d9b1f445807ad),
  UINT64_C(0x29b61a324a673399),
  UINT64_C(0x9c4e994550765f89)
};

static volatile double result_fp[N] __attribute__((aligned(64)));
static volatile uint64_t result_add[N] __attribute__((aligned(64)));
int main(void) {
  unsigned long vlenb, misa;
  asm volatile("csrr %0, vlenb" : "=r"(vlenb));
  asm volatile("csrr %0, misa" : "=r"(misa));
  if (vlenb != 32 || !(misa & (1UL<<21)) || !(misa & (1UL<<3))) return 101;
  if (__riscv_vsetvl_e8m1((size_t)-1) != 32) return 102;
  if (__riscv_vsetvl_e32m4((size_t)-1) != 32) return 103;
  if (__riscv_vsetvl_e64m1((size_t)-1) != 4) return 104;
  if (__riscv_vsetvl_e64m2((size_t)-1) != 8) return 105;
  if (__riscv_vsetvl_e64m8((size_t)-1) != 32) return 106;
  unsigned checks = 0;
#ifdef ARC_SATURN_INJECT_FP64_FAULT
  const double alpha = 0.25;
#else
  const double alpha = 0.5;
#endif
  for (size_t i=0; i<N;) {
    size_t vl = __riscv_vsetvl_e64m1(N-i);
    vfloat64m1_t vx = __riscv_vle64_v_f64m1(x+i,vl);
    vfloat64m1_t vy = __riscv_vle64_v_f64m1(y+i,vl);
    vfloat64m1_t vr = __riscv_vfmacc_vf_f64m1(vy,alpha,vx,vl);
    __riscv_vse64_v_f64m1((double *)result_fp+i,vr,vl);
    i += vl;
  }
  for (unsigned i=0; i<N; ++i) {
    union {double d; uint64_t u;} bits;
    bits.d = result_fp[i];
    if (bits.u != expected_fp[i]) return 111;
    ++checks;
  }
  for (size_t i=0; i<N;) {
    size_t vl = __riscv_vsetvl_e64m8(N-i);
    vfloat64m8_t vx = __riscv_vle64_v_f64m8(x+i,vl);
    vfloat64m8_t vy = __riscv_vle64_v_f64m8(y+i,vl);
    vfloat64m8_t vr = __riscv_vfmacc_vf_f64m8(vy,alpha,vx,vl);
    __riscv_vse64_v_f64m8((double *)result_fp+i,vr,vl);
    i += vl;
  }
  for (unsigned i=0; i<N; ++i) {
    union {double d; uint64_t u;} bits;
    bits.d = result_fp[i];
    if (bits.u != expected_fp[i]) return 112;
    ++checks;
  }
  for (size_t i=0; i<N;) {
    size_t vl = __riscv_vsetvl_e64m2(N-i);
    vuint64m2_t va = __riscv_vle64_v_u64m2(a+i,vl);
    vuint64m2_t vb = __riscv_vle64_v_u64m2(b+i,vl);
    vuint64m2_t vr = __riscv_vadd_vv_u64m2(va,vb,vl);
    __riscv_vse64_v_u64m2((uint64_t *)result_add+i,vr,vl);
    i += vl;
  }
  for (unsigned i=0; i<N; ++i) {
    if (result_add[i] != expected_add[i]) return 113;
    ++checks;
  }
  if (checks != 111) return 114;
  printf("SATURN_ARCH_OK vlenb=%lu fp64=74 i64=37 checks=%u\n",vlenb,checks);
  return 0;
}
