cmake_minimum_required(VERSION 3.20)
project(DarknessGameSetup LANGUAGES CXX)

# The launcher's game setup as a command-line tool (scripts/build-all.ps1 runs
# it before the rest of the project is built).
set(DARKNESS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(darkness_game_setup
    main.cpp
    "${DARKNESS_ROOT}/runtime/runtime_game_setup.cpp")
target_include_directories(darkness_game_setup PRIVATE
    "${DARKNESS_ROOT}/runtime"
    "${DARKNESS_ROOT}/external/ReXGlue/thirdparty/tomlplusplus/include")
target_compile_features(darkness_game_setup PRIVATE cxx_std_17)
target_compile_options(darkness_game_setup PRIVATE /EHsc)
target_compile_definitions(darkness_game_setup PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(darkness_game_setup PRIVATE bcrypt)
set_property(TARGET darkness_game_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
