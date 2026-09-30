# Cross-compile for x86_64 Windows (MSVC ABI) from Linux with clang-cl and
# lld-link, using the MSVC CRT + Windows SDK that linux/setup-toolchain.sh
# splats with xwin (build/linux-toolchain/sysroot, or ESTACADO_XWIN_SYSROOT).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(DEFINED ENV{ESTACADO_XWIN_SYSROOT})
  set(XWIN_SYSROOT "$ENV{ESTACADO_XWIN_SYSROOT}")
else()
  set(XWIN_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../build/linux-toolchain/sysroot")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_xwin_includes
  "${XWIN_SYSROOT}/crt/include"
  "${XWIN_SYSROOT}/sdk/include/ucrt"
  "${XWIN_SYSROOT}/sdk/include/um"
  "${XWIN_SYSROOT}/sdk/include/shared"
  "${XWIN_SYSROOT}/sdk/include/winrt"
  "${XWIN_SYSROOT}/sdk/include/cppwinrt")
set(_xwin_compile_flags "")
set(_xwin_rc_flags "")
foreach(dir IN LISTS _xwin_includes)
  string(APPEND _xwin_compile_flags " /imsvc \"${dir}\"")
  string(APPEND _xwin_rc_flags " -I \"${dir}\"")
endforeach()

# Append so a caller's -DCMAKE_<LANG>_FLAGS_INIT (e.g. -march) is kept. Do not
# pass -DCMAKE_<LANG>_FLAGS: it replaces the INIT value and drops the sysroot.
string(APPEND CMAKE_C_FLAGS_INIT " ${_xwin_compile_flags}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_xwin_compile_flags}")
set(CMAKE_RC_FLAGS_INIT "${_xwin_rc_flags}")

set(_xwin_link_flags
  "/libpath:\"${XWIN_SYSROOT}/crt/lib/x86_64\" /libpath:\"${XWIN_SYSROOT}/sdk/lib/um/x86_64\" /libpath:\"${XWIN_SYSROOT}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_xwin_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_link_flags}")

set(CMAKE_FIND_ROOT_PATH "${XWIN_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# xwin ships release CRT libs only; build and probe Release.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
