cmake_minimum_required(VERSION 3.20)
project(ChipyardArcilatorDriver LANGUAGES C CXX)

set(ARCILATOR_SOURCE "" CACHE FILEPATH "External pinned Arcilator driver source")
set(CIRCT_SDK "" CACHE PATH "Installed firtool-1.162.0 SDK; defaults to firtool on PATH")
option(ARCILATOR_DRIVER_ENABLE_JIT "Include the SDK JIT driver support" ON)
option(ARCILATOR_DRIVER_ENABLE_FST "Include FST driver support; SDK default is OFF" OFF)
set(ARCILATOR_DRIVER_CXX11_ABI "1" CACHE STRING "SDK libstdc++ C++11 ABI, 0 or 1")
set_property(CACHE ARCILATOR_DRIVER_CXX11_ABI PROPERTY STRINGS 0 1)

if(NOT EXISTS "${ARCILATOR_SOURCE}")
  message(FATAL_ERROR "Set ARCILATOR_SOURCE to the external pinned arcilator.cpp")
endif()
get_filename_component(ARCILATOR_SOURCE "${ARCILATOR_SOURCE}" REALPATH)
file(SHA256 "${ARCILATOR_SOURCE}" driver_source_sha256)
if(driver_source_sha256 STREQUAL "1b420f21a26721c8ba3fa6f7b332a72dcb1e5b6af4c05a6b02b66ff82f3934d7")
  set(driver_source_variant original)
elseif(driver_source_sha256 STREQUAL "7bb2c3e8367771ea2fe7567ed47632d37d31a6600484d185aa108528c31833f2")
  set(driver_source_variant bytecode)
else()
  message(FATAL_ERROR "Arcilator source does not match the pinned original or bytecode patch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARCILATOR_SOURCE}")
if(NOT ARCILATOR_DRIVER_CXX11_ABI MATCHES "^[01]$")
  message(FATAL_ERROR "ARCILATOR_DRIVER_CXX11_ABI must be 0 or 1")
endif()

if(NOT CIRCT_SDK)
  find_program(driver_reference_firtool NAMES firtool REQUIRED)
  get_filename_component(driver_reference_bin "${driver_reference_firtool}" DIRECTORY)
  get_filename_component(CIRCT_SDK "${driver_reference_bin}/.." REALPATH)
else()
  get_filename_component(CIRCT_SDK "${CIRCT_SDK}" REALPATH)
endif()
execute_process(COMMAND "${CIRCT_SDK}/bin/firtool" --version
  RESULT_VARIABLE driver_version_result OUTPUT_VARIABLE driver_sdk_version
  ERROR_VARIABLE driver_version_error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT driver_version_result EQUAL 0 OR NOT driver_sdk_version MATCHES "CIRCT firtool-1\\.162\\.0($|[\r\n])")
  message(FATAL_ERROR "Expected CIRCT firtool-1.162.0 SDK: ${driver_version_error}")
endif()
execute_process(COMMAND "${CIRCT_SDK}/bin/llvm-config" --version
  RESULT_VARIABLE driver_llvm_result OUTPUT_VARIABLE driver_llvm_version
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT driver_llvm_result EQUAL 0 OR NOT driver_llvm_version STREQUAL "24.0.0git")
  message(FATAL_ERROR "Expected the matching LLVM24 SDK")
endif()

set(LLVM_DIR "${CIRCT_SDK}/lib/cmake/llvm")
set(MLIR_DIR "${CIRCT_SDK}/lib/cmake/mlir")
find_package(CIRCT REQUIRED CONFIG PATHS "${CIRCT_SDK}/lib/cmake/circt" NO_DEFAULT_PATH)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# The imported SDK executable is also named arcilator; keep our target distinct.
add_executable(arcilator-driver "${ARCILATOR_SOURCE}")
set_target_properties(arcilator-driver PROPERTIES OUTPUT_NAME arcilator
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(arcilator-driver PRIVATE cxx_std_17)
target_compile_options(arcilator-driver PRIVATE -O3 -DNDEBUG -fno-rtti -fno-exceptions)
target_compile_definitions(arcilator-driver PRIVATE
  _GLIBCXX_USE_CXX11_ABI=${ARCILATOR_DRIVER_CXX11_ABI})
set(driver_jit_libraries)
set(driver_llvm_components Support TargetParser)
if(ARCILATOR_DRIVER_ENABLE_JIT)
  target_compile_definitions(arcilator-driver PRIVATE ARCILATOR_ENABLE_JIT)
  list(APPEND driver_jit_libraries CIRCTArcJITRuntime MLIRExecutionEngine)
  list(APPEND driver_llvm_components native OrcJIT codegen)
endif()
if(ARCILATOR_DRIVER_ENABLE_FST)
  target_compile_definitions(arcilator-driver PRIVATE CIRCT_LIBFST_ENABLED=1)
endif()
llvm_map_components_to_libnames(driver_llvm_libraries ${driver_llvm_components})
target_link_libraries(arcilator-driver PRIVATE
  CIRCTArc CIRCTArcToLLVM CIRCTArcTransforms CIRCTArcilator CIRCTCombToArith
  CIRCTConvertToArcs CIRCTEmitTransforms CIRCTExportArc CIRCTHWTransforms
  CIRCTLLHD CIRCTLTL CIRCTOMTransforms CIRCTSeqToSV CIRCTSeqTransforms
  CIRCTSimTransforms CIRCTSupport CIRCTTransforms CIRCTVerif
  MLIRArithDialect MLIRBuiltinToLLVMIRTranslation MLIRBytecodeWriter
  MLIRControlFlowDialect MLIRDLTIDialect MLIRFuncDialect MLIRFuncInlinerExtension
  MLIRIndexDialect MLIRLLVMIRTransforms MLIRLLVMToLLVMIRTranslation MLIRParser
  MLIRSCFDialect MLIRTargetLLVMIRExport MLIRTargetLLVMIRImport MLIRUBDialect
  ${driver_jit_libraries} ${driver_llvm_libraries})

include("${CMAKE_CURRENT_LIST_DIR}/Provenance.cmake")
add_custom_command(TARGET arcilator-driver POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
    "-DDRIVER_BINARY=$<TARGET_FILE:arcilator-driver>"
    "-DDRIVER_CONFIG=${CMAKE_BINARY_DIR}/arcilator-driver-config.json"
    "-DDRIVER_RECORD=${CMAKE_BINARY_DIR}/arcilator-driver-build.json"
    -P "${CMAKE_CURRENT_LIST_DIR}/RecordBuild.cmake"
  VERBATIM)
