cmake_minimum_required(VERSION 3.16)
project(tapto-vnc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Version stamp ----------------------------------------------------------
# Puts the version and the originating commit into the binary, so --version and
# the first line of every trace name the build that produced them. Regenerated
# on each build, not at configure time; see cmake/GitVersion.cmake.
set(TAPTO_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(TAPTO_VERSION_HEADER "${TAPTO_GENERATED_DIR}/tapto/version.h")

set(TAPTO_VERSION_ARGS
  -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
  -DIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
  -DOUT_FILE=${TAPTO_VERSION_HEADER}
  -DPROJECT_VERSION=${PROJECT_VERSION}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
)

# Once at configure time as well, so the header exists before anything asks an
# IDE or a generator to parse the sources that include it.
execute_process(COMMAND ${CMAKE_COMMAND} ${TAPTO_VERSION_ARGS})

add_custom_target(tapto_version ALL
  COMMAND ${CMAKE_COMMAND} ${TAPTO_VERSION_ARGS}
  BYPRODUCTS ${TAPTO_VERSION_HEADER}
  COMMENT "Stamping version ${PROJECT_VERSION}"
  VERBATIM
)

# --- Dependencies -----------------------------------------------------------
# OpenSSL: DES for VNC authentication, TLS for wss:// transport. Found below,
#          after libtapto: the library locates it (Shining Light's MSVC layout
#          included) or, on a Windows host with none at all, fetches a pinned
#          prebuilt, and leaves OPENSSL_ROOT_DIR in the cache for the find
#          here. A find_package before the library would fail on such a host
#          before the library got its chance.
# zlib:    ZRLE encoding.

# Fetched at configure time, pinned to the same tags tapto-code uses so the
# two projects stay on one version of each dependency.
include(FetchContent)

# zlib: auto-downloaded via FetchContent (zlib ZRLE encoding)
FetchContent_Declare(zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
  GIT_SHALLOW TRUE
)
# Its example programs would otherwise register as tests of ours.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# And its install rules would put its headers, import libraries and DLL into
# any tree that installs this project (the Windows installer packages exactly
# that tree), with absolute destinations CPack refuses outright. zlib is
# linked statically and private to us; nothing of it is installed.
set(SKIP_INSTALL_ALL ON)
FetchContent_MakeAvailable(zlib)

# Provide compatibility alias for existing ZLIB::ZLIB usage
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB INTERFACE IMPORTED)
  set_target_properties(ZLIB::ZLIB PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR}"
    INTERFACE_LINK_LIBRARIES zlibstatic)
endif()

# --- libtapto ---------------------------------------------------------------
# The config store, the secret resolver, provider resolution and the three
# provider clients with the agent loop inside are libtapto's, shared with
# tapto-code and tapto-word. It is fetched from its own repository at a tag --
# it used to be a copy vendored here, one of five kept byte-identical by hand
# -- and it brings its own pins for nlohmann_json and cpp-httplib, exported on
# the `tapto::tapto` target, so they are not repeated here. OpenSSL was found
# above; the library reuses that.
#
# Bump LIBTAPTO_TAG to take a library change. -DLIBTAPTO_TAG=main builds
# against an unreleased library, and -DFETCHCONTENT_SOURCE_DIR_LIBTAPTO=<path>
# against a working tree, which is how a change spanning both is developed.
#
# What is NOT in the library, and is this program's own: the screen-control
# tools (src/computer_tools.cpp), the VNC session, the MCP server, the CLI, and
# src/ui.cpp, which supplies the bodies for the tapto::ui functions the library
# declares -- a terminal status line here.
#
# Its unit tests run under ctest with ours.
set(LIBTAPTO_BUILD_TESTS ON CACHE BOOL "" FORCE)
set(LIBTAPTO_TAG "v0.1.0" CACHE STRING "libtapto version to build against")
FetchContent_Declare(libtapto
  GIT_REPOSITORY https://github.com/centlakestefan/libtapto.git
  GIT_TAG        ${LIBTAPTO_TAG}
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libtapto)

# OpenSSL for this directory's own targets (tapto_rfb). Imported targets are
# scoped to the directory that created them, so the library's OpenSSL::SSL is
# not visible here; the package is found again, from the cache entries the
# library left behind.
find_package(OpenSSL REQUIRED)

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
# For tapto/log.h and the json/httplib/OpenSSL it brings; PRIVATE, since none
# of them appear in this target's own headers.
target_link_libraries(tapto_vnc PRIVATE tapto::tapto)
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
# The screen-control tools, the MCP server and ui.cpp, on top of libtapto's
# provider clients and agent loop.
add_library(tapto_agent STATIC
  src/computer_tools.cpp
  src/frame_index.cpp
  src/input_map.cpp
  src/mcp_server.cpp
  src/ui.cpp
)
target_include_directories(tapto_agent PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${TAPTO_GENERATED_DIR}")
add_dependencies(tapto_agent tapto_version)
# The library exports nlohmann_json, httplib, OpenSSL, Threads and, on Windows,
# advapi32/crypt32, together with CPPHTTPLIB_OPENSSL_SUPPORT; PUBLIC because
# main.cpp includes the provider headers, which include httplib and json.
target_link_libraries(tapto_agent PUBLIC tapto_vnc tapto::tapto)

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

# --- Tests ------------------------------------------------------------------
# Plain executables with assertions of their own, so `ctest` needs no test
# framework. Run with: ctest --test-dir build --output-on-failure
#
# The library's own test (test-libtapto) is registered by libtapto's own CMakeLists.
enable_testing()

# The screen-control tools' status-line labels, through the ToolSpec::display
# hooks. Links tapto_agent, which carries the library's ui bodies (ui.cpp).
add_executable(test-labels tests/test_labels.cpp)
target_link_libraries(test-labels PRIVATE tapto_agent)
if(MSVC)
  target_compile_options(test-labels PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(test-labels PRIVATE -Wall -Wextra)
endif()
add_test(NAME labels COMMAND test-labels)
