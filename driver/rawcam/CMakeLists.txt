cmake_minimum_required(VERSION 3.20)

set(APP_NAME wp81rawcam)
project(${APP_NAME} C)

# Legacy (non-PnP, unloadable) WDM driver for Windows Phone 8.1 (ARM), built
# with LLVM (clang-cl + lld-link, see CMakePresets.json).
#   - ARM kernel import libs: Windows Kits 8.1 (WDK)
#   - wdm.h and the shared headers: Windows Phone Kits 8.1 (um)
#   - qcvcm8930.sys import library generated from qcvcm8930.def
set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/8.1" CACHE PATH "Windows Kits 8.1 root")
set(PHONE_KIT_ROOT "C:/Program Files (x86)/Windows Phone Kits/8.1" CACHE PATH "Windows Phone Kits 8.1 root")
set(LLVM_LIB "C:/Program Files/LLVM/bin/llvm-lib.exe" CACHE FILEPATH "llvm-lib")

set(CMAKE_C_FLAGS_DEBUG "/Od")
set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CREATE_CONSOLE_EXE "")

set(QCVCM_LIB "${CMAKE_BINARY_DIR}/qcvcm8930.lib")
add_custom_command(
    OUTPUT "${QCVCM_LIB}"
    COMMAND "${LLVM_LIB}" /nologo /machine:arm "/def:${CMAKE_CURRENT_SOURCE_DIR}/qcvcm8930.def" "/out:${QCVCM_LIB}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qcvcm8930.def"
    COMMENT "Generating qcvcm8930.lib"
)
add_custom_target(qcvcm8930_lib DEPENDS "${QCVCM_LIB}")

file(GLOB SOURCES CONFIGURE_DEPENDS "src/*.c")
add_executable(${APP_NAME} ${SOURCES})
add_dependencies(${APP_NAME} qcvcm8930_lib)

set_target_properties(${APP_NAME} PROPERTIES
    SUFFIX ".sys"
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_compile_definitions(${APP_NAME} PRIVATE
    _ARM_=1
    _KERNEL_MODE=1
    WINNT=1
    NTDDI_VERSION=0x06030000
    _WIN32_WINNT=0x0603
    WINVER=0x0603
    DEPRECATE_DDK_FUNCTIONS=1
)

target_include_directories(${APP_NAME} SYSTEM PRIVATE
    "${WDK_ROOT}/Include/km/crt"
    "${PHONE_KIT_ROOT}/Include/um"
    "${PHONE_KIT_ROOT}/Include/um/minwin"
)

target_compile_options(${APP_NAME} PRIVATE
    /W4
    /Zl                 # no default CRT library references
    /GS-                # no stack cookies (no GsDriverEntry to initialise them)
    /Gy
    /Z7
    /clang:-mno-implicit-float   # no compiler-generated VFP/NEON use in kernel code
)

target_link_directories(${APP_NAME} PRIVATE
    "${WDK_ROOT}/Lib/winv6.3/km/arm"
)

target_link_options(${APP_NAME} PRIVATE
    /DRIVER
    /SUBSYSTEM:NATIVE,6.03
    /ENTRY:DriverEntry
    /NODEFAULTLIB
    /MACHINE:ARM
    /DEBUG
    /OPT:REF
    /OPT:ICF
    /INCREMENTAL:NO
    /MANIFEST:NO
    /TSAWARE:NO
)

target_link_libraries(${APP_NAME} PRIVATE
    ntoskrnl.lib
    hal.lib
    BufferOverflowFastFailK.lib
    "${QCVCM_LIB}"
)
