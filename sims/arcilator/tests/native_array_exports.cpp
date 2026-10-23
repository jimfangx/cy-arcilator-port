#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace chipyard_arc;
#include "native_array_exports_api.h"
static unsigned checks = 0;
static void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
static uint64_t rng = 0xa2bdc07819ef3456ull;
static uint32_t random_word() {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}
static bool bit(const svLogicVecVal *p, unsigned pos) {
  return ((p[pos / 32].aval & ~p[pos / 32].bval) >> (pos % 32)) & 1;
}
int main(int argc, char **argv) {
  try {
    for (unsigned round = 0; round < 3; ++round) {
      auto m = create_model(argc, argv);
      m->initialize();
      std::array<svScope, 2> scopes;
      for (unsigned which = 0; which < 2; ++which) {
        std::string path = std::string("ArrayExportHarness/") +
                           (which ? "second" : "first") + "/rtl/rtl/child";
        scopes[which] = svGetScopeFromName(path.c_str());
        check(scopes[which], "missing array export scope");
        svSetScope(scopes[which]);
        check(array_saved() == int(which + 1) * 7,
              "array capture initialization/instance mismatch");
      }
      for (unsigned iter = 0; iter < 256; ++iter)
        for (unsigned which = 0; which < 2; ++which) {
          unsigned id = which + 1, n = which ? 5 : 3, width = which ? 129 : 65,
                   words = (width + 31) / 32, outwidth = width + 64,
                   outwords = (outwidth + 31) / 32;
          svSetScope(scopes[which]);
          std::vector<int32_t> values(n);
          std::vector<char> result(n, 0x55);
          std::vector<int16_t> work(n), expected(n);
          uint32_t sum = 0;
          for (unsigned i = 0; i < n; ++i) {
            values[i] = random_word();
            work[i] = int16_t(random_word());
            expected[i] =
                i == n - 1
                    ? work[i]
                    : int16_t(uint32_t(work[i]) + uint32_t(values[i]) + id);
            sum ^= uint32_t(values[i]) + uint32_t(int32_t(expected[i]));
          }
          if (argc > 1 && std::strcmp(argv[1], "+array-export-fault") == 0)
            sum ^= 1;
          check(uint32_t(array_atoms(values.data(), result.data(),
                                     work.data())) == sum,
                "array atom return mismatch");
          for (unsigned i = 0; i < n; ++i) {
            check(result[i] ==
                      (i == n - 1 ? 0
                                  : char(uint32_t(values[i]) ^ (id * 17 + i))),
                  "array output order/default/padding mismatch");
            check(work[i] == expected[i],
                  "array inout order/retained element mismatch");
          }
          check(uint32_t(array_saved()) == sum,
                "array write/capture read mismatch");
          check(m->signal(which ? "second" : "first").get() == sum,
                "array capture write not published to RTL");
          std::vector<svLogicVecVal> a(6 * words),
              b(6 * outwords, {0xdeadbeefu, 0xfeedfacfu});
          std::array<svBitVecVal, 6> c, oldc;
          for (auto &v : a)
            v = {random_word(), random_word()};
          for (unsigned i = 0; i < 6; ++i)
            c[i] = oldc[i] = random_word();
          array_vectors(a.data(), b.data(), c.data());
          for (unsigned i = 0; i < 6; ++i) {
            uint64_t high = 0x123456789abcdef0ull ^ (i + id);
            for (unsigned pos = 0; pos < outwords * 32; ++pos) {
              bool want = i != 5 && pos < outwidth &&
                          (pos < width ? bit(a.data() + i * words, pos)
                                       : ((high >> (pos - width)) & 1));
              check(((b[i * outwords + pos / 32].aval >> (pos % 32)) & 1) ==
                        want,
                    "array vector element/bit/padding mismatch");
            }
            for (unsigned j = 0; j < outwords; ++j)
              check(b[i * outwords + j].bval == 0,
                    "array vector output unknown bits nonzero");
            unsigned low = 0;
            for (unsigned pos = 0; pos < 13; ++pos)
              if (bit(a.data() + i * words, pos))
                low |= 1u << pos;
            unsigned want = (oldc[i] ^ (i == 5 ? 0 : low ^ (i + id))) & 8191;
            check(c[i] == want, "array13 inout/padding/default mismatch");
          }
          std::array<svBitVecVal, 14> zero;
          zero.fill(0xa5a5a5a5u);
          array_zero(zero.data() + 1);
          unsigned zero_words = which ? 10 : 9;
          for (unsigned i = 0; i < zero.size(); ++i)
            check(zero[i] == (i > 0 && i <= zero_words ? 0u : 0xa5a5a5a5u),
                  "equal-width instance array layout/padding/canary mismatch");
          std::array<svLogic, 3> logic = {svLogic(iter & 3),
                                          svLogic((iter + 1) & 3),
                                          svLogic((iter + 2) & 3)},
                                 logicwork = {svLogic((iter + 3) & 3),
                                              svLogic(iter & 3),
                                              svLogic((iter + 1) & 3)};
          auto oldlogic = logicwork;
          std::array<svBit, 3> bits = {9, 9, 9};
          check(array_bits(logic.data(), bits.data(), logicwork.data()) ==
                    int((logic[1] == 1) ^ id),
                "array logic return mismatch");
          for (unsigned i = 0; i < 3; ++i) {
            check(bits[i] == unsigned(logic[i] == 1),
                  "array scalar logic encoding mismatch");
            check(logicwork[i] ==
                      unsigned((oldlogic[i] == 1) ^ (logic[i] == 1)),
                  "array scalar logic inout encoding mismatch");
          }
          char byte = char(random_word());
          short narrow = short(random_word());
          int integer = int(random_word());
          long long wide = static_cast<long long>(
              (static_cast<unsigned long long>(random_word()) << 32) |
              random_word());
          if (iter == 0) {
            byte = char(0x80);
            narrow = short(0x8000);
            integer = int(0x80000000u);
            wide = std::numeric_limits<long long>::min();
          }
          if (iter == 1) {
            byte = char(0x7f);
            narrow = short(0x7fff);
            integer = 0x7fffffff;
            wide = std::numeric_limits<long long>::max();
          }
          char byte_out = 0;
          long long wide_out = 0;
          unsigned long long signed_sum =
              static_cast<unsigned long long>(wide) +
              static_cast<unsigned long long>(integer) +
              static_cast<unsigned long long>(narrow) +
              static_cast<unsigned long long>(static_cast<signed char>(byte));
          check(static_cast<unsigned long long>(signed_atoms(
                    byte, narrow, integer, wide, &byte_out, &wide_out)) ==
                    signed_sum,
                "signed primitive C prototype/value mismatch");
          check(byte_out == char(byte ^ id),
                "signed byte output prototype/value mismatch");
          check(static_cast<unsigned long long>(wide_out) ==
                    (static_cast<unsigned long long>(wide) ^ id),
                "signed longint output prototype/value mismatch");
          unsigned char ubyte = static_cast<unsigned char>(random_word()),
                        ubyte_out = 0;
          unsigned short ushort = static_cast<unsigned short>(random_word());
          unsigned int uint = random_word();
          unsigned long long ulong =
                                 (static_cast<unsigned long long>(random_word())
                                  << 32) |
                                 random_word(),
                             ulong_out = 0;
          if (iter == 0) {
            ubyte = 255;
            ushort = 65535;
            uint = 0xffffffffu;
            ulong = ~0ull;
          }
          if (iter == 1) {
            ubyte = 0;
            ushort = 0;
            uint = 0;
            ulong = 0;
          }
          check(unsigned_atoms(ubyte, ushort, uint, ulong, &ubyte_out,
                               &ulong_out) == ulong + uint + ushort + ubyte,
                "unsigned primitive C prototype/value mismatch");
          check(ubyte_out == static_cast<unsigned char>(ubyte ^ id),
                "unsigned byte output prototype/value mismatch");
          check(ulong_out == (ulong ^ id),
                "unsigned longint output prototype/value mismatch");
          std::array<unsigned long long, 3> longs, oldlongs;
          std::array<long long, 3> longout;
          for (unsigned i = 0; i < 3; ++i) {
            longs[i] = (static_cast<unsigned long long>(random_word()) << 32) |
                       random_word();
            oldlongs[i] = static_cast<unsigned long long>(wide) + i;
          }
          auto worklongs = oldlongs;
          array_longs(longs.data(), longout.data(), worklongs.data());
          for (unsigned i = 0; i < 3; ++i) {
            check(static_cast<unsigned long long>(longout[i]) ==
                      (longs[i] ^ (i + id)),
                  "longint array C prototype/output mismatch");
            check(worklongs[i] == oldlongs[i] + longs[i],
                  "longint array C prototype/inout mismatch");
          }
          if (iter < 16) {
            std::string first = "left-" + std::to_string(iter),
                        last = iter % 2 ? "" : "UTF8-\xc3\xa9";
            const char *names[2] = {first.c_str(), last.c_str()};
            const char *changed[2] = {"old-A", "old-B"};
            const void *handles[2] = {
                reinterpret_cast<void *>(
                    uintptr_t(0x123456789abcdef0ull + iter)),
                iter % 2 ? nullptr
                         : reinterpret_cast<void *>(
                               uintptr_t(0xfedcba9876543210ull))};
            void *copied[2] = {nullptr, nullptr};
            std::string left = first + ":old-A", right = "old-B:" + last;
            check(array_pointers(names, changed, handles, copied) ==
                      int(left.size() + right.size() + id),
                  "array string return mismatch");
            check(changed[0] == left && changed[1] == right,
                  "array string inout order/lifetime mismatch");
            check(copied[0] == handles[1] && copied[1] == handles[0],
                  "array chandle output lost pointer bits/order");
            first.assign(200, 'X');
            last.assign(200, 'Y');
            const char *read[2] = {nullptr, nullptr};
            void *readhandles[2] = {nullptr, nullptr};
            array_saved_pointers(read, readhandles);
            check(read[0] == left && read[1] == right,
                  "array captured string storage/borrowed buffer mismatch");
            check(readhandles[0] == copied[0] && readhandles[1] == copied[1],
                  "array captured chandle mismatch");
          }
        }
      for (unsigned which = 0; which < 2; ++which) {
        svSetScope(scopes[which]);
        int32_t values[5]{};
        char result[5]{};
        int16_t work[5]{};
        for (unsigned nullarg = 0; nullarg < 3; ++nullarg) {
          bool rejected = false;
          try {
            array_atoms(nullarg == 0 ? nullptr : values,
                        nullarg == 1 ? nullptr : result,
                        nullarg == 2 ? nullptr : work);
          } catch (const std::invalid_argument &) {
            rejected = true;
          }
          check(rejected, "null array formal accepted");
        }
      }
      m->signal("enable").set(1);
      m->signal("seed").set(0x87654321);
      m->signal("clock").set(0);
      m->advance(0);
      m->signal("clock").set(1);
      m->advance(1000);
      for (unsigned which = 0; which < 2; ++which) {
        svSetScope(scopes[which]);
        check(uint32_t(array_saved()) == (0x87654321u ^ (which + 1)),
              "RTL write/capture read mismatch after array export");
      }
    }
    std::printf(
        "PASS: %u native fixed-array export ABI/scope/coherence checks\n",
        checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "ARRAY_EXPORT_FAILURE: %s\n", error.what());
    return 1;
  }
}
