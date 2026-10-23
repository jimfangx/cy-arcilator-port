// Send a real NIC DMA packet through SimNetwork's C++ switch back to this NIC.
// Use WithSimNetwork_LoopbackNICRocketConfig, with no external TAP device.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mmio.h"
#include "nic.h"

static unsigned char sent[64] __attribute__((aligned(64)));
static unsigned char received[64] __attribute__((aligned(64)));

int main(void) {
  uint64_t mac = nic_macaddr();
  for (unsigned i = 0; i < sizeof(sent); ++i) sent[i] = (unsigned char)(i ^ 0x5a);
  // IceNIC includes two alignment bytes before the Ethernet header.
  memcpy(sent + 2, &mac, 6);
  memcpy(sent + 8, &mac, 6);
  sent[14] = 0x88;
  sent[15] = 0xb5;
  asm volatile ("fence iorw, iorw" ::: "memory");
  while (nic_recv_req_avail() == 0) {}
  reg_write64(SIMPLENIC_RECV_REQ, (uintptr_t)received);
  nic_send(sent, sizeof(sent));
  while (nic_recv_comp_avail() == 0) {}
  unsigned length = reg_read16(SIMPLENIC_RECV_COMP);
  asm volatile ("fence iorw, iorw" ::: "memory");
  if (length != sizeof(sent) || memcmp(sent, received, sizeof(sent))) return 1;
  puts("PASS: SimNetwork DMA transmit, route, receive, and payload comparison");
  return 0;
}
