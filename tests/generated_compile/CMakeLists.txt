cmake_minimum_required(VERSION 3.20)
project(DarknessGeneratedCompile LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../generated/ppc")
set(SIMDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/ppc_recomp.*.cpp")
list(APPEND GENERATED_SOURCES "${PPC_DIR}/ppc_func_mapping.cpp")

if(NOT EXISTS "${PPC_DIR}/ppc_recomp.0.cpp")
    message(FATAL_ERROR "Generated PPC source is absent. Run scripts/recomp.ps1 first.")
endif()

function(configure_generated_target target)
    target_include_directories(${target} PRIVATE "${PPC_DIR}" "${SIMDE_DIR}")
    target_compile_options(${target} PRIVATE /arch:AVX)
endfunction()

# Smallest translation-unit probe. Its result distinguishes source/toolchain
# failures from project-wide scale or resource constraints.
add_library(darkness_generated_probe OBJECT "${PPC_DIR}/ppc_recomp.0.cpp")
configure_generated_target(darkness_generated_probe)

# Full object-only pass. There is deliberately no runtime, executable, or link
# target at this stage.
add_library(darkness_generated_all OBJECT ${GENERATED_SOURCES})
configure_generated_target(darkness_generated_all)
