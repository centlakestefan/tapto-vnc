# libtapto -- the code the tapto programs share.
#
# One static library: the config store and secret resolver, provider
# resolution, the three provider clients with the agent loop inside, and the
# helpers they lean on (UTF-8 sanitising, tool images, logging). Nothing here
# knows which program it is in. In particular tapto::ui is DECLARED here and
# DEFINED by each program: the library calls set_status() and friends, and what
# those do -- a terminal row, an SSE frame, nothing -- is the program's call.
#
# Consumed with add_subdirectory(libtapto) and target_link_libraries(... tapto).
# The dependencies are fetched here, pinned, unless the consumer has already
# made the same targets available -- so a program can bring its own pins or let
# this file bring them.

cmake_minimum_required(VERSION 3.16)
project(libtapto VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies -----------------------------------------------------------
# Bump these in one place, here, and every program moves together.
include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  set(JSON_Install OFF CACHE INTERNAL "")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# HTTPS out to the providers. OpenSSL must be installed and findable; on
# Windows point CMake at it with -DOPENSSL_ROOT_DIR=C:/path/to/openssl.
#
# -DLIBTAPTO_STATIC_SSL=ON links OpenSSL and zlib statically where a static
# build of them exists. Strawberry Perl's MinGW ships import libraries only
# (its libcrypto.a is all __imp_ stubs), so on that toolchain this stays off
# and the installer ships the DLLs instead, found by file(GET_RUNTIME_DEPENDENCIES).
option(LIBTAPTO_STATIC_SSL "Link OpenSSL (and zlib) statically" OFF)
if(LIBTAPTO_STATIC_SSL)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL REQUIRED)

if(NOT TARGET httplib::httplib)
  set(HTTPLIB_REQUIRE_OPENSSL ON CACHE INTERNAL "")
  # Header-only and private to us: its install rules would otherwise put
  # httplib.h and a cmake package into every consumer's install tree.
  set(HTTPLIB_INSTALL OFF CACHE INTERNAL "")
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(httplib)
endif()

find_package(Threads REQUIRED)

# --- Library ----------------------------------------------------------------
add_library(tapto STATIC
  src/claude.cpp
  src/openai.cpp
  src/gemini.cpp
  src/config.cpp
  src/paths.cpp
  src/secret.cpp
  src/provider.cpp
  src/encoding.cpp
  src/fstools.cpp
  src/certs.cpp
  src/tool_image.cpp
)
add_library(tapto::tapto ALIAS tapto)

target_include_directories(tapto PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(tapto PUBLIC
  CPPHTTPLIB_OPENSSL_SUPPORT
)

target_link_libraries(tapto PUBLIC
  nlohmann_json::nlohmann_json
  httplib::httplib
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)

# CredReadW / CredFree, for the `wincred:` secret scheme in tapto/secret.h;
# crypt32 for the user's trusted-root store in tapto/certs.h.
if(WIN32)
  target_link_libraries(tapto PUBLIC advapi32 crypt32)
endif()

if(MSVC)
  target_compile_options(tapto PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(tapto PRIVATE -Wall -Wextra)
endif()

# A MinGW build otherwise depends on libstdc++-6.dll and friends being on PATH,
# and picks up whichever copy it finds first -- a different GCC's, on a machine
# with Git Bash installed, which fails with STATUS_ENTRYPOINT_NOT_FOUND. Every
# program linking this library inherits the flags, so a Release directory can
# be moved and run as it is. With static OpenSSL the whole runtime goes static
# (winpthread included) and the executable depends on system DLLs only.
if(MINGW)
  if(LIBTAPTO_STATIC_SSL)
    target_link_options(tapto PUBLIC -static)
    # Static OpenSSL was built against zlib and pulls in the Windows crypto
    # and socket APIs itself.
    find_library(ZLIB_STATIC NAMES libz.a z)
    if(ZLIB_STATIC)
      target_link_libraries(tapto PUBLIC ${ZLIB_STATIC})
    endif()
    target_link_libraries(tapto PUBLIC ws2_32 crypt32)
  else()
    target_link_options(tapto PUBLIC -static-libgcc -static-libstdc++)
  endif()
endif()

# --- Tests ------------------------------------------------------------------
# Plain executables with assertions of their own, so ctest needs no framework.
# Only built when this is the top-level project or the consumer asks, so a
# program's build does not pick up the library's tests as its own.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_libtapto_top_level ON)
else()
  set(_libtapto_top_level OFF)
endif()
option(LIBTAPTO_BUILD_TESTS "Build libtapto's unit tests" ${_libtapto_top_level})

if(LIBTAPTO_BUILD_TESTS)
  enable_testing()

  # The tests link the library, and the library's provider clients reference
  # tapto::ui, so the test binary supplies a do-nothing ui.
  add_executable(test-libtapto
    test/test_libtapto.cpp
    test/ui_null.cpp
  )
  target_link_libraries(test-libtapto PRIVATE tapto)
  if(MSVC)
    target_compile_options(test-libtapto PRIVATE /W4 /EHsc /utf-8)
  else()
    target_compile_options(test-libtapto PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME libtapto COMMAND test-libtapto)
endif()
