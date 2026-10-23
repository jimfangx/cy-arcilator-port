#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
using namespace chipyard_arc;
struct Channel { unsigned chip, initializes = 0, ticks = 0; };
static std::array<Channel,3> channels{{{0},{1},{2}}};
static void check(bool good, const char *message) {
  if (!good) throw std::runtime_error(message);
}
extern "C" void *memory_init(int chip, long long size, long long word,
    long long line, long long ids, long long hz, long long base, int addresses) {
  check(chip >= 0 && chip < 3, "generated CHIP_ID changed");
  auto &channel = channels[chip];
  const unsigned words[] = {8,4,16};
  if (word != words[chip]) {
    std::fprintf(stderr,"generated WORD_SIZE mismatch chip=%d expected=%u actual=%lld\n",chip,words[chip],word);
    throw std::runtime_error("generated SimDRAM WORD_SIZE callback argument changed");
  }
  check(size == 268435456 && line == 64 && ids == 4 && hz == 500000000 &&
    base == 2147483648LL && addresses == 32 && ++channel.initializes == 1,
    "generated SimDRAM parameter widths/order or lazy init changed");
  return &channel;
}
extern "C" void memory_tick(void *opaque, unsigned char reset,
    unsigned char ar_valid, unsigned char *ar_ready, long long ar_addr,
    int ar_id, int ar_size, int ar_len,
    unsigned char aw_valid, unsigned char *aw_ready, long long aw_addr,
    int aw_id, int aw_size, int aw_len,
    unsigned char w_valid, unsigned char *w_ready, int w_strb,
    const svOpenArrayHandle w_data, unsigned char w_last,
    unsigned char *r_valid, unsigned char r_ready, int *r_id, int *r_resp,
    const svOpenArrayHandle r_data, unsigned char *r_last,
    unsigned char *b_valid, unsigned char b_ready, int *b_id, int *b_resp) {
  auto &channel = *static_cast<Channel *>(opaque);
  check(channel.chip < 3 && opaque == &channels[channel.chip] &&
        channel.initializes == 1 && !reset,"generated channel/reset changed");
  ++channel.ticks;
  check(ar_valid && uint64_t(ar_addr) == 0x81234567 && ar_id == 3 &&
    ar_size == 3 && ar_len == 1 && aw_valid && uint64_t(aw_addr) == 0x82345678 &&
    aw_id == 4 && aw_size == 3 && aw_len == 2 && w_valid && w_strb == 255 &&
    w_last && r_ready && !b_ready,"generated AXI callback inputs changed");
  check(svSize(w_data,1)==8 && svSize(r_data,1)==8,"WORD_SIZE changed generated payload width");
  for(unsigned i=0;i<8;++i) {
    check(*static_cast<const uint8_t *>(svGetArrElemPtr1(w_data,i))==17+i,"generated write byte order");
    *static_cast<uint8_t *>(svGetArrElemPtr1(r_data,i))=0xee - i;
  }
  *ar_ready=*aw_ready=*w_ready=*r_valid=*r_last=*b_valid=1;
  *r_id=5;*r_resp=2;*b_id=6;*b_resp=1;
}
int main() {
  try {
    auto runtime=create_model(0,nullptr);runtime->initialize();
    auto step=[&](unsigned time,unsigned clock,unsigned reset) {
      runtime->advance(time,[&] {runtime->signal("clock").set(clock);runtime->signal("reset").set(reset);});
    };
    auto counts=[&](unsigned initialized,unsigned ticks) {
      for(auto &channel:channels)check(channel.initializes==initialized&&channel.ticks==ticks,"generated callback edge/reset count changed");
    };
    auto outputs=[&](unsigned valid) {
      for(unsigned i=0;i<3;++i) {
        auto prefix="c"+std::to_string(i)+"_";
        check(runtime->signal(prefix+"axi_r_valid").get()==valid&&runtime->signal(prefix+"axi_b_valid").get()==valid,"generated response-valid commit/reset changed");
        if(valid)check(runtime->signal(prefix+"axi_r_bits_id").get()==5&&runtime->signal(prefix+"axi_r_bits_resp").get()==2&&runtime->signal(prefix+"axi_r_bits_data").get()==0xe7e8e9eaebecedeeULL&&runtime->signal(prefix+"axi_b_bits_id").get()==6,"generated response data/id/byte order changed");
      }
    };
    step(0,0,1);step(1,1,1);counts(0,0);outputs(0);
    step(2,1,0);counts(0,0);
    step(3,0,0);step(4,1,0);step(5,1,0);counts(1,1);outputs(1);
    step(6,0,0);step(7,1,0);counts(1,2);
    step(8,0,1);step(9,1,1);counts(1,2);outputs(0);
    step(10,0,0);step(11,1,0);counts(1,3);outputs(1);
    std::puts("PASS generated native CIRCT SimDRAM: all39ports,3instances,WORD_SIZE default8/override4/override16,9callbacks,reset/edge/response byte order");
    return 0;
  }catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
