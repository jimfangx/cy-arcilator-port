// Native implementation of the registered wrapper in spiketile.v.
#include "runtime.h"
extern "C" void spike_tile_reset(int) __attribute__((weak));
extern "C" void spike_tile(
    int hartid, char *isa, int pmpregions, int icache_sets, int icache_ways,
    int dcache_sets, int dcache_ways, char *cacheable, char *uncacheable,
    char *readonly_uncacheable, char *executable, int icache_sourceids,
    int dcache_sourceids, long long int tcm_base, long long int tcm_size,
    long long int reset_vector, long long int ipc, long long int cycle,
    long long int *insns_retired, char debug, char mtip, char msip, char meip,
    char seip, unsigned char icache_a_ready, unsigned char *icache_a_valid,
    long long int *icache_a_address, long long int *icache_a_sourceid,
    unsigned char icache_d_valid, long long int icache_d_sourceid,
    long long int icache_d_data_0, long long int icache_d_data_1,
    long long int icache_d_data_2, long long int icache_d_data_3,
    long long int icache_d_data_4, long long int icache_d_data_5,
    long long int icache_d_data_6, long long int icache_d_data_7,
    unsigned char dcache_a_ready, unsigned char *dcache_a_valid,
    long long int *dcache_a_address, long long int *dcache_a_sourceid,
    unsigned char *dcache_a_state_old, unsigned char *dcache_a_state_new,
    unsigned char dcache_b_valid, long long int dcache_b_address,
    long long int dcache_b_source, int dcache_b_param,
    unsigned char dcache_c_ready, unsigned char *dcache_c_valid,
    long long int *dcache_c_address, long long int *dcache_c_source,
    int *dcache_c_param, unsigned char *dcache_c_voluntary,
    unsigned char *dcache_c_has_data, long long int *dcache_c_data_0,
    long long int *dcache_c_data_1, long long int *dcache_c_data_2,
    long long int *dcache_c_data_3, long long int *dcache_c_data_4,
    long long int *dcache_c_data_5, long long int *dcache_c_data_6,
    long long int *dcache_c_data_7, unsigned char dcache_d_valid,
    unsigned char dcache_d_has_data, unsigned char dcache_d_grantack,
    long long int dcache_d_sourceid, long long int dcache_d_data_0,
    long long int dcache_d_data_1, long long int dcache_d_data_2,
    long long int dcache_d_data_3, long long int dcache_d_data_4,
    long long int dcache_d_data_5, long long int dcache_d_data_6,
    long long int dcache_d_data_7, unsigned char mmio_a_ready,
    unsigned char *mmio_a_valid, long long int *mmio_a_address,
    long long int *mmio_a_data, unsigned char *mmio_a_store, int *mmio_a_size,
    unsigned char mmio_d_valid, long long int mmio_d_data,
    unsigned char tcm_a_valid, long long int tcm_a_address,
    long long int tcm_a_data, int tcm_a_mask, int tcm_a_opcode, int tcm_a_size,
    unsigned char *tcm_d_valid, unsigned char tcm_d_ready,
    long long int *tcm_d_data) __attribute__((weak));

namespace chipyard_arc {
namespace {
class Spike : public Binding {
  long long int insns_retired = 0;
  unsigned char icache_a_valid = 0;
  long long int icache_a_address = 0;
  long long int icache_a_sourceid = 0;
  unsigned char dcache_a_valid = 0;
  long long int dcache_a_address = 0;
  long long int dcache_a_sourceid = 0;
  unsigned char dcache_a_state_old = 0;
  unsigned char dcache_a_state_new = 0;
  unsigned char dcache_c_valid = 0;
  long long int dcache_c_address = 0;
  long long int dcache_c_sourceid = 0;
  int dcache_c_param = 0;
  unsigned char dcache_c_voluntary = 0;
  unsigned char dcache_c_has_data = 0;
  long long int dcache_c_data_0 = 0;
  long long int dcache_c_data_1 = 0;
  long long int dcache_c_data_2 = 0;
  long long int dcache_c_data_3 = 0;
  long long int dcache_c_data_4 = 0;
  long long int dcache_c_data_5 = 0;
  long long int dcache_c_data_6 = 0;
  long long int dcache_c_data_7 = 0;
  unsigned char mmio_a_valid = 0;
  long long int mmio_a_address = 0;
  long long int mmio_a_data = 0;
  unsigned char mmio_a_store = 0;
  int mmio_a_size = 0;
  unsigned char tcm_d_valid = 0;
  long long int tcm_d_data = 0;

public:
  using Binding::Binding;
  void initialize() override {
    if (!spike_tile || !spike_tile_reset)
      throw std::runtime_error("Spike tile driver is not linked");
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    if (get("reset")) {
      spike_tile_reset(number("HARTID"));
      insns_retired = 0;
      icache_a_valid = 0;
      icache_a_address = 0;
      icache_a_sourceid = 0;
      dcache_a_valid = 0;
      dcache_a_address = 0;
      dcache_a_sourceid = 0;
      dcache_a_state_old = 0;
      dcache_a_state_new = 0;
      dcache_c_valid = 0;
      dcache_c_address = 0;
      dcache_c_sourceid = 0;
      dcache_c_param = 0;
      dcache_c_voluntary = 0;
      dcache_c_has_data = 0;
      dcache_c_data_0 = 0;
      dcache_c_data_1 = 0;
      dcache_c_data_2 = 0;
      dcache_c_data_3 = 0;
      dcache_c_data_4 = 0;
      dcache_c_data_5 = 0;
      dcache_c_data_6 = 0;
      dcache_c_data_7 = 0;
      mmio_a_valid = 0;
      mmio_a_address = 0;
      mmio_a_data = 0;
      mmio_a_store = 0;
      mmio_a_size = 0;
      tcm_d_valid = 0;
      tcm_d_data = 0;
    } else {
      spike_tile(
          number("HARTID"), const_cast<char *>(text("ISA").c_str()),
          number("PMPREGIONS"), number("ICACHE_SETS"), number("ICACHE_WAYS"),
          number("DCACHE_SETS"), number("DCACHE_WAYS"),
          const_cast<char *>(text("CACHEABLE").c_str()),
          const_cast<char *>(text("UNCACHEABLE").c_str()),
          const_cast<char *>(text("READONLY_UNCACHEABLE").c_str()),
          const_cast<char *>(text("EXECUTABLE").c_str()),
          number("ICACHE_SOURCEIDS"), number("DCACHE_SOURCEIDS"),
          number("TCM_BASE"), number("TCM_SIZE"), get("reset_vector"),
          get("ipc"), get("cycle"), &insns_retired, get("debug"), get("mtip"),
          get("msip"), get("meip"), get("seip"), get("icache_a_ready"),
          &icache_a_valid, &icache_a_address, &icache_a_sourceid,
          get("icache_d_valid"), get("icache_d_sourceid"),
          get("icache_d_data_0"), get("icache_d_data_1"),
          get("icache_d_data_2"), get("icache_d_data_3"),
          get("icache_d_data_4"), get("icache_d_data_5"),
          get("icache_d_data_6"), get("icache_d_data_7"), get("dcache_a_ready"),
          &dcache_a_valid, &dcache_a_address, &dcache_a_sourceid,
          &dcache_a_state_old, &dcache_a_state_new, get("dcache_b_valid"),
          get("dcache_b_address"), get("dcache_b_source"),
          get("dcache_b_param"), get("dcache_c_ready"), &dcache_c_valid,
          &dcache_c_address, &dcache_c_sourceid, &dcache_c_param,
          &dcache_c_voluntary, &dcache_c_has_data, &dcache_c_data_0,
          &dcache_c_data_1, &dcache_c_data_2, &dcache_c_data_3,
          &dcache_c_data_4, &dcache_c_data_5, &dcache_c_data_6,
          &dcache_c_data_7, get("dcache_d_valid"), get("dcache_d_has_data"),
          get("dcache_d_grantack"), get("dcache_d_sourceid"),
          get("dcache_d_data_0"), get("dcache_d_data_1"),
          get("dcache_d_data_2"), get("dcache_d_data_3"),
          get("dcache_d_data_4"), get("dcache_d_data_5"),
          get("dcache_d_data_6"), get("dcache_d_data_7"), get("mmio_a_ready"),
          &mmio_a_valid, &mmio_a_address, &mmio_a_data, &mmio_a_store,
          &mmio_a_size, get("mmio_d_valid"), get("mmio_d_data"),
          get("tcm_a_valid"), get("tcm_a_address"), get("tcm_a_data"),
          get("tcm_a_mask"), get("tcm_a_opcode"), get("tcm_a_size"),
          &tcm_d_valid, get("tcm_d_ready"), &tcm_d_data);
    }
    stage("insns_retired", uint64_t(insns_retired));
    stage("icache_a_valid", uint64_t(icache_a_valid));
    stage("icache_a_address", uint64_t(icache_a_address));
    stage("icache_a_sourceid", uint64_t(icache_a_sourceid));
    stage("dcache_a_valid", uint64_t(dcache_a_valid));
    stage("dcache_a_address", uint64_t(dcache_a_address));
    stage("dcache_a_sourceid", uint64_t(dcache_a_sourceid));
    stage("dcache_a_state_old", uint64_t(dcache_a_state_old));
    stage("dcache_a_state_new", uint64_t(dcache_a_state_new));
    stage("dcache_c_valid", uint64_t(dcache_c_valid));
    stage("dcache_c_address", uint64_t(dcache_c_address));
    stage("dcache_c_sourceid", uint64_t(dcache_c_sourceid));
    stage("dcache_c_param", uint64_t(dcache_c_param));
    stage("dcache_c_voluntary", uint64_t(dcache_c_voluntary));
    stage("dcache_c_has_data", uint64_t(dcache_c_has_data));
    stage("dcache_c_data_0", uint64_t(dcache_c_data_0));
    stage("dcache_c_data_1", uint64_t(dcache_c_data_1));
    stage("dcache_c_data_2", uint64_t(dcache_c_data_2));
    stage("dcache_c_data_3", uint64_t(dcache_c_data_3));
    stage("dcache_c_data_4", uint64_t(dcache_c_data_4));
    stage("dcache_c_data_5", uint64_t(dcache_c_data_5));
    stage("dcache_c_data_6", uint64_t(dcache_c_data_6));
    stage("dcache_c_data_7", uint64_t(dcache_c_data_7));
    stage("mmio_a_valid", uint64_t(mmio_a_valid));
    stage("mmio_a_address", uint64_t(mmio_a_address));
    stage("mmio_a_data", uint64_t(mmio_a_data));
    stage("mmio_a_store", uint64_t(mmio_a_store));
    stage("mmio_a_size", uint64_t(mmio_a_size));
    stage("tcm_d_valid", uint64_t(tcm_d_valid));
    stage("tcm_d_data", uint64_t(tcm_d_data));
  }

public:
  constexpr BindingCallbackContract<Spike> native_callback_contract() const {
    return reviewed_base_callback_contract<Spike>();
  }
};
} // namespace
void register_spike_bindings() {
#define ARC_SPIKE_MODEL(type, name)                                            \
  register_binding(name, [](Runtime &r, const std::string &n) {                \
    return make_binding_instance<type>(r, n);                                  \
  });
#include "models.def"
#undef ARC_SPIKE_MODEL
}
} // namespace chipyard_arc
