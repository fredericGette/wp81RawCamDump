cmake_minimum_required(VERSION 3.20)

set(APP_NAME wp81RawCamDump)
project(${APP_NAME} CXX)

# Windows Phone 8.1 ARM console application, built with LLVM (clang-cl +
# lld-link, see CMakePresets.json). No STL: plain C-style C++ only, so the
# old VC11 WPSDK library headers are not needed beyond the CRT.
file(GLOB SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(${APP_NAME} ${SOURCES})

target_compile_definitions(${APP_NAME} PRIVATE
    _WIN32_WINNT=0x0603
    WINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(${APP_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../driver/common"
)

target_compile_options(${APP_NAME} PRIVATE
    /W4
    /clang:-fno-sized-deallocation
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include\""
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include/mincore\""
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include/minwin\""
    "SHELL:/external:I \"C:/Program Files (x86)/Microsoft Visual Studio 11.0/VC/WPSDK/include\""
    /external:W0
)

set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

target_link_libraries(${APP_NAME} PRIVATE
    mincore.lib
)
