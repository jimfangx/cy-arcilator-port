#include "runtime.h"
#include <algorithm>
#include <deque>
#include <iomanip>
#include <sstream>
extern "C" uint64_t chipyard_arc_signal_read(uint64_t, uint64_t, uint64_t,
                                             uint64_t, uint64_t);
namespace chipyard_arc {
const Runtime::ExportTarget &
Runtime::find_export(const std::string &name) const {
  auto scope = svGetNameFromScope(svGetScope());
  if (!scope)
    throw std::runtime_error("DPI export requires a selected SV scope: " +
                             name);
  auto found = dpi_exports.find({scope, name});
  if (found == dpi_exports.end())
    throw std::runtime_error("no DPI export " + name + " in " + scope);
  return found->second;
}
void Runtime::add_export(const std::string &scope, const std::string &name,
                         ExportKernel entry, unsigned formals,
                         unsigned result_bits, std::vector<unsigned> widths,
                         std::vector<bool> references,
                         std::vector<Signal> captures, std::string layout) {
  if (!entry || widths.size() != references.size() || formals > widths.size() ||
      widths.size() - formals != captures.size() ||
      std::any_of(widths.begin(), widths.end(),
                  [](unsigned n) { return !n || n > (1u << 24); }) ||
      result_bits > (1u << 24))
    throw std::runtime_error("invalid native DPI export layout: " + name);
  for (unsigned i = formals; i < widths.size(); i++)
    if (!references[i])
      throw std::runtime_error("DPI capture is not a reference: " + name);
  owned_scope(scope);
  if (!dpi_exports
           .emplace(std::make_pair(scope, name),
                    ExportTarget{entry, std::move(layout), formals, result_bits,
                                 std::move(widths), std::move(references),
                                 std::move(captures)})
           .second)
    throw std::runtime_error("duplicate DPI export " + name + " in " + scope);
}
unsigned Runtime::export_width(const std::string &name, unsigned index) const {
  const auto &target = find_export(name);
  if (index < target.formals)
    return target.widths[index];
  if (index == target.formals && target.result_bits)
    return target.result_bits;
  throw std::out_of_range("native DPI export argument index");
}
const std::string &Runtime::export_layout(const std::string &name) const {
  // Layout identity survives optimizers merging identical packet functions.
  // Equal logical widths can still require different C array element padding.
  return find_export(name).layout;
}
void Runtime::invoke_export(const std::string &name,
                            std::vector<ExportBytes> &values) {
  if (!initialized)
    throw std::runtime_error("initialize the model before calling DPI exports");
  const auto &target = find_export(name);
  if (values.size() != target.formals + unsigned(target.result_bits != 0))
    throw std::runtime_error("native DPI export argument count mismatch");
  for (unsigned i = 0; i < values.size(); i++)
    if (values[i].size() != (export_width(name, i) + 7) / 8)
      throw std::runtime_error("native DPI export buffer width mismatch");
  if (!evaluation_depth && !export_depth)
    settle();
  struct Depth {
    unsigned &value;
    Depth(unsigned &v) : value(v) { ++value; }
    ~Depth() { --value; }
  };
  struct Temporary {
    unsigned argument, width;
    uint64_t handle = 0, version = 0;
    std::unique_ptr<Binding> binding;
  };
  {
    Depth guard(export_depth);
    auto scope = svGetNameFromScope(svGetScope());
    DpiContext context(owned_scope(scope));
    std::vector<uint8_t *> packet(
        target.widths.size() + unsigned(target.result_bits != 0), nullptr);
    std::vector<uint64_t> refs;
    std::deque<Temporary> temporary;
    for (unsigned i = 0; i < target.widths.size(); i++) {
      auto width = target.widths[i];
      if (i < target.formals && !target.references[i]) {
        packet[i] = values[i].data();
        continue;
      }
      uint64_t handle = 0;
      if (i >= target.formals)
        handle = target.captures[i - target.formals].get();
      else {
        temporary.emplace_back();
        auto &item = temporary.back();
        item.argument = i;
        item.width = width;
        item.binding = make_binding(*this, "ArcSignalMemory");
        item.binding->ports = {
            {"handle", {reinterpret_cast<uint8_t *>(&item.handle), 64}},
            {"version", {reinterpret_cast<uint8_t *>(&item.version), 64}}};
        std::ostringstream initial;
        initial << "0x" << std::hex << std::setfill('0');
        for (auto it = values[i].rbegin(); it != values[i].rend(); ++it)
          initial << std::setw(2) << unsigned(*it);
        item.binding->parameters = {{"BITS", std::to_string(width)},
                                    {"INIT", initial.str()}};
        item.binding->initialize();
        handle = item.handle;
      }
      if (!handle)
        throw std::runtime_error("uninitialized native DPI export capture");
      refs.insert(refs.end(), {handle, 0, 0, width});
    }
    if (target.result_bits)
      packet[target.widths.size()] = values[target.formals].data();
    target.entry(packet.data(), refs.data());
    for (auto &item : temporary) {
      auto &data = values[item.argument];
      for (unsigned offset = 0; offset < item.width; offset += 64) {
        auto count = std::min(64u, item.width - offset);
        auto word = chipyard_arc_signal_read(item.handle, 0, offset,
                                             item.width - offset, count);
        for (unsigned byte = 0; byte < (count + 7) / 8; byte++)
          data[offset / 8 + byte] = word >> (byte * 8);
      }
    }
  }
  if (!evaluation_depth && !export_depth)
    settle();
}
} // namespace chipyard_arc
