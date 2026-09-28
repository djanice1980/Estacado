cmake_minimum_required(VERSION 3.25)

project(xenos_shader_disasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(REXGLUE_ROOT "${PROJECT_ROOT}/external/ReXGlue")
set(REXGLUE_BUILD_ROOT "${REXGLUE_ROOT}/out/build/win-amd64")
set(REXGLUE_LIBRARY_ROOT "${REXGLUE_ROOT}/out/win-amd64/Release")

add_executable(xenos_shader_disasm
  main.cpp
  "${REXGLUE_ROOT}/src/graphics/pipeline/shader/shader.cpp"
  "${REXGLUE_ROOT}/src/graphics/format/ucode.cpp"
  "${REXGLUE_ROOT}/src/graphics/pipeline/shader/translator.cpp"
  "${REXGLUE_ROOT}/src/graphics/pipeline/shader/translator_disasm.cpp"
)

target_include_directories(xenos_shader_disasm PRIVATE
  "${REXGLUE_ROOT}/include"
  "${REXGLUE_ROOT}"
  "${REXGLUE_BUILD_ROOT}/include"
  "${REXGLUE_ROOT}/thirdparty/simde"
  "${REXGLUE_ROOT}/thirdparty/simde/simde"
  "${REXGLUE_ROOT}/thirdparty/fmt/include"
  "${REXGLUE_ROOT}/thirdparty/spdlog/include"
  "${REXGLUE_BUILD_ROOT}/thirdparty/sdl3/include-revision"
  "${REXGLUE_ROOT}/thirdparty/sdl3/include"
  "${REXGLUE_ROOT}/thirdparty/dxc/include"
  "${REXGLUE_ROOT}/thirdparty/xxHash"
  "${REXGLUE_BUILD_ROOT}/thirdparty/snappy"
  "${REXGLUE_ROOT}/thirdparty/snappy"
  "${REXGLUE_ROOT}/thirdparty/tomlplusplus/include"
)

target_compile_definitions(xenos_shader_disasm PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  REX_PLATFORM_WINDOWS=1
  REX_HAS_D3D12=1
  REXGLUE_BUILD_CONFIG="Release"
  SPDLOG_COMPILED_LIB
  SPDLOG_FMT_EXTERNAL
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(xenos_shader_disasm PRIVATE
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t
  )
endif()

target_link_libraries(xenos_shader_disasm PRIVATE
  "${REXGLUE_LIBRARY_ROOT}/rexruntime.lib"
  "${REXGLUE_LIBRARY_ROOT}/fmt.lib"
  "${REXGLUE_LIBRARY_ROOT}/spdlog.lib"
)

add_custom_command(TARGET xenos_shader_disasm POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${REXGLUE_LIBRARY_ROOT}/rexruntime.dll"
          "$<TARGET_FILE_DIR:xenos_shader_disasm>"
)
