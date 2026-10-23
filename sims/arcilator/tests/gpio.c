// MMIO and physical input smoke test for Chipyard's GPIORocketConfig.
#include <stdint.h>
#include <stdio.h>

int main(void) {
  volatile uint32_t *gpio = (volatile uint32_t *)0x10010000UL;
  gpio[2] = 0; // Disable pad drivers; the harness ties these pads to zero.
  gpio[1] = 15;
  gpio[3] = 10;
  asm volatile ("fence iorw, iorw" ::: "memory");
  if ((gpio[1] & 15) != 15 || (gpio[3] & 15) != 10 || (gpio[0] & 15) != 0)
    return 1;
  puts("PASS: GPIO registers and attached physical inputs");
  return 0;
}
