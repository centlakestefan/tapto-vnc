cmake_minimum_required(VERSION 3.16)
project(tapto-vnc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Dependencies -----------------------------------------------------------
# OpenSSL: DES for VNC authentication, TLS for wss:// transport.
# zlib:    ZRLE encoding.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# Fetched at configure time, pinned to the same tags tapto-code uses so the
# two projects stay on one version of each dependency.
include(FetchContent)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

set(HTTPLIB_REQUIRE_OPENSSL ON CACHE INTERNAL "")
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.15.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json httplib)

# --- Windows defines --------------------------------------------------------
# <windows.h>, pulled in by <winsock2.h>, defines min and max as macros, which
# breaks all seventeen std::min / std::max calls in the vendored RFB sources
# under MSVC ("error C2589: '(': illegal token on right side of '::'"). MinGW
# does not define them, so this only appears in an MSVC build.
# WIN32_LEAN_AND_MEAN additionally keeps the old winsock.h out, which is what
# you want when winsock2.h is in use.
#
# Declared after FetchContent on purpose, so it applies to our targets and not
# to the fetched dependencies, which manage their own Windows defines.
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- Vendored RFB stack -----------------------------------------------------
# Client-side sources copied from the cpp-vnc project (the VNC server, its
# tests, and its example binaries are deliberately not vendored). Kept as a
# separate target so upstream's warnings don't drown out ours.
file(GLOB TAPTO_RFB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rfb/*.cpp")

add_library(tapto_rfb STATIC ${TAPTO_RFB_SOURCES})
target_include_directories(tapto_rfb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rfb")
target_link_libraries(tapto_rfb PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

if(WIN32)
  target_link_libraries(tapto_rfb PUBLIC ws2_32 crypt32)
endif()

if(MSVC)
  target_compile_options(tapto_rfb PRIVATE /W0 /EHsc /utf-8)
else()
  target_compile_options(tapto_rfb PRIVATE -w)
endif()

# --- VNC session ------------------------------------------------------------
add_library(tapto_vnc STATIC
  src/vnc_session.cpp
  src/vmware_console.cpp
)
target_link_libraries(tapto_vnc PRIVATE
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
)
target_compile_definitions(tapto_vnc PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_include_directories(tapto_vnc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Separate call: CMake only accepts SYSTEM directly after the target name, so
# it cannot be mixed into the call above. stb_image_write.h is compiled into
# our own TU, and without SYSTEM its warnings surface under our -Wall/-Wextra.
target_include_directories(tapto_vnc SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
# PRIVATE: rfb types stay out of tapto_vnc's public headers by design, so
# downstream targets never need the RFB include path.
target_link_libraries(tapto_vnc PRIVATE tapto_rfb)

if(MSVC)
  target_compile_options(tapto_vnc PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(tapto_vnc PRIVATE -Wall -Wextra)
endif()

# --- Agent ------------------------------------------------------------------
# The provider client, tool registry and context are reused from tapto-code;
# ui.cpp is a small local stand-in for its terminal module.
add_library(tapto_agent STATIC
  src/claude.cpp
  src/computer_tools.cpp
  src/gemini.cpp
  src/openai.cpp
  src/config.cpp
  src/input_map.cpp
  src/paths.cpp
  src/tool_image.cpp
  src/ui.cpp
)
target_include_directories(tapto_agent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# httplib and nlohmann/json are PUBLIC because claude.h includes both in its
# own interface, so anything including it needs their headers and the matching
# OpenSSL feature macro.
target_link_libraries(tapto_agent
  PUBLIC tapto_vnc nlohmann_json::nlohmann_json httplib::httplib
         OpenSSL::SSL OpenSSL::Crypto
)
target_compile_definitions(tapto_agent PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)

if(MSVC)
  target_compile_options(tapto_agent PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(tapto_agent PRIVATE -Wall -Wextra)
endif()

# --- Executables ------------------------------------------------------------
add_executable(tapto-vnc src/main.cpp)
target_link_libraries(tapto-vnc PRIVATE tapto_agent)

add_executable(vnc-smoketest src/vnc_smoketest.cpp)
target_link_libraries(vnc-smoketest PRIVATE tapto_vnc)

foreach(exe tapto-vnc vnc-smoketest)
  if(MSVC)
    target_compile_options(${exe} PRIVATE /W4 /EHsc /utf-8)
  else()
    target_compile_options(${exe} PRIVATE -Wall -Wextra)
  endif()
endforeach()

install(TARGETS tapto-vnc vnc-smoketest RUNTIME DESTINATION bin)
