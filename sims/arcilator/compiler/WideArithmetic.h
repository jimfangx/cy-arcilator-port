// Provenance contract shared by native arithmetic and DPI event sensitivity.
#pragma once
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/SymbolTable.h"
#include <optional>
#include <string>

namespace chipyard_arc_compiler {
inline bool ownsWideShift(mlir::func::FuncOp callee, mlir::IntegerType type,
                          unsigned mode) {
  return callee && mode < 3 && callee.isExternal() && type.getWidth() > 1024 &&
      callee->getAttrOfType<mlir::UnitAttr>("chipyard.native_pure_wide_shift") &&
      callee->getAttrOfType<mlir::UnitAttr>("chipyard.no_output_readback") &&
      callee.getFunctionType() == mlir::FunctionType::get(type.getContext(),
                                                        {type, type}, {type}) &&
      callee.getSymName() == "chipyard_arc_wide_shift_" +
          std::to_string(mode) + "_" + std::to_string(type.getWidth());
}

inline std::optional<unsigned>
wideShiftMode(circt::sim::DPICallOp call,
              mlir::SymbolTableCollection &symbols) {
  if (call.getClock() || call.getEnable() || call.getInputs().size() != 2 ||
      call.getNumResults() != 1 ||
      !call.getCalleeAttr().getValue().starts_with("chipyard_arc_wide_shift_"))
    return {};
  auto type = mlir::dyn_cast<mlir::IntegerType>(call.getResult(0).getType());
  if (!type || call.getInputs()[0].getType() != type ||
      call.getInputs()[1].getType() != type)
    return {};
  auto callee = symbols.lookupNearestSymbolFrom<mlir::func::FuncOp>(
      call, call.getCalleeAttr());
  for (unsigned mode = 0; mode < 3; ++mode)
    if (ownsWideShift(callee, type, mode))
      return mode;
  return {};
}
} // namespace chipyard_arc_compiler
