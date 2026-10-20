cmake_minimum_required(VERSION 3.28)
project(ChipyardVerilog LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(CIRCT REQUIRED CONFIG)

add_library(NativeImportVerilog STATIC
  MooreToCore.cpp
  Statements.cpp CaptureAnalysis.cpp Expressions.cpp HierarchicalNames.cpp
  Types.cpp TimingControls.cpp ScanStrings.cpp AssertionExpr.cpp Structure.cpp
  FormatStrings.cpp ImportVerilog.cpp)
target_include_directories(NativeImportVerilog PUBLIC
  "${CIRCT_INCLUDE_DIR}"
  "${SLANG_SOURCE}/include"
  "${SLANG_BUILD}/source"
  "${SLANG_BUILD}/_deps/fmt-src/include"
  "${SLANG_SOURCE}/external")
target_compile_options(NativeImportVerilog PRIVATE -fno-rtti -fno-exceptions)
target_link_libraries(NativeImportVerilog PUBLIC
  CIRCTComb CIRCTDebug CIRCTHW CIRCTLLHD CIRCTLLHDTransforms CIRCTLTL
  CIRCTMoore CIRCTMooreToCore CIRCTMooreTransforms CIRCTSV CIRCTSeq
  CIRCTSeqTransforms CIRCTSupport CIRCTTransforms CIRCTVerif MLIRFuncDialect
  MLIRSCFDialect MLIRTranslateLib slang)

add_executable(arcilator-verilog circt-verilog.cpp)
target_compile_options(arcilator-verilog PRIVATE -fno-rtti -fno-exceptions)
target_link_libraries(arcilator-verilog PRIVATE
  CIRCTComb CIRCTDebug CIRCTHW NativeImportVerilog CIRCTLLHD
  CIRCTLLHDTransforms CIRCTMoore CIRCTMooreToCore CIRCTMooreTransforms
  CIRCTSeq CIRCTSeqTransforms CIRCTSim CIRCTSupport CIRCTTransforms CIRCTVerif
  MLIRArithDialect MLIRBytecodeReader MLIRBytecodeWriter MLIRControlFlowDialect
  MLIRFuncDialect MLIRFuncInlinerExtension MLIRIR MLIRLLVMDialect
  MLIRLLVMIRTransforms MLIRParser MLIRSCFDialect MLIRSupport MLIRUBDialect)
