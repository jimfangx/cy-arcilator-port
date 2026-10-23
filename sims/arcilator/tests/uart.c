// Exercise the actual UART pins and SimUART DPI on the default Rocket config.
// Feed two bytes, '?' then 'Z', to simulator stdin. The first may arrive before
// software enables the receiver. The harness's original baud rate is preserved.
#include <stdint.h>
#include <stdio.h>

int main(void) {
  volatile uint32_t *uart = (volatile uint32_t *)0x10020000UL;
  uart[2] = 1; // TX enable.
  uart[3] = 1; // RX enable.
  while (uart[0] & 0x80000000U) {}
  uart[0] = 'A';
  for (;;) {
    uint32_t rx = uart[1];
    if (!(rx & 0x80000000U) && (rx & 255U) == 'Z') break;
  }
  puts("PASS: SimUART physical transmit and receive");
  return 0;
}
