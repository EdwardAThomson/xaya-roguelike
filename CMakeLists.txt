cmake_minimum_required (VERSION 3.14)
project (xayaroguelike VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

add_compile_options (-Wall -pedantic)

# Needed on Windows for glog; harmless elsewhere.
add_compile_definitions (GLOG_NO_ABBREVIATED_SEVERITIES)

include (GNUInstallDirs)
include (FetchContent)
find_package (PkgConfig REQUIRED)

# Path to libxayagame source tree (for headers in full build).
set (XAYAGAME_SOURCE_DIR "$ENV{HOME}/Explore/libxayagame"
     CACHE PATH "Path to libxayagame source tree")

# ============================================================
# Dependencies
# ============================================================

# sqlite3
pkg_check_modules (SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
set_property (TARGET PkgConfig::SQLITE3 APPEND PROPERTY
  INTERFACE_COMPILE_DEFINITIONS
    SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK
)

# glog
find_package (glog QUIET)
if (NOT glog_FOUND)
  FetchContent_Declare (glog
    GIT_REPOSITORY https://github.com/google/glog.git
    GIT_TAG v0.7.1
    GIT_SHALLOW TRUE
  )
  set (WITH_GTEST OFF CACHE BOOL "" FORCE)
  set (WITH_GFLAGS OFF CACHE BOOL "" FORCE)
  set (BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable (glog)
endif ()

# gflags
find_package (gflags QUIET)
if (NOT gflags_FOUND)
  FetchContent_Declare (gflags
    GIT_REPOSITORY https://github.com/gflags/gflags.git
    GIT_TAG v2.2.2
    GIT_SHALLOW TRUE
  )
  set (BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable (gflags)
endif ()

# jsoncpp
pkg_check_modules (JSONCPP IMPORTED_TARGET jsoncpp)
if (NOT JSONCPP_FOUND)
  FetchContent_Declare (jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG 1.9.6
    GIT_SHALLOW TRUE
  )
  set (JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set (JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable (jsoncpp)
endif ()

# GTest
find_package (GTest REQUIRED)
enable_testing ()

# Check for installed libxayagame.
pkg_check_modules (XAYAGAME IMPORTED_TARGET libxayagame)
pkg_check_modules (XAYAUTIL IMPORTED_TARGET libxayautil)

# jsonrpcstub for RPC stub generation (Phase 3).
find_program (JSONRPCSTUB jsonrpcstub)

# ============================================================
# Schema embedding — two variants
# ============================================================

# 1) For the full library (uses xaya::SQLiteDatabase via schema.hpp)
add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/schema.cpp
  COMMAND ${CMAKE_COMMAND}
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/schema.cpp
    -DINPUT0=${CMAKE_CURRENT_SOURCE_DIR}/schema_head.cpp
    -DINPUT1=${CMAKE_CURRENT_SOURCE_DIR}/schema.sql
    -DINPUT2=${CMAKE_CURRENT_SOURCE_DIR}/schema_tail.cpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/concat_files.cmake
  DEPENDS schema_head.cpp schema.sql schema_tail.cpp
)

# 2) For tests (raw C function, no xaya dependency)
add_custom_command (
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/schema_test.cpp
  COMMAND ${CMAKE_COMMAND}
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/schema_test.cpp
    -DINPUT0=${CMAKE_CURRENT_SOURCE_DIR}/schema_test_head.cpp
    -DINPUT1=${CMAKE_CURRENT_SOURCE_DIR}/schema.sql
    -DINPUT2=${CMAKE_CURRENT_SOURCE_DIR}/schema_test_tail.cpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/concat_files.cmake
  DEPENDS schema_test_head.cpp schema.sql schema_test_tail.cpp
)

# ============================================================
# Unit tests (no libxayagame dependency)
# ============================================================

add_executable (roguelike_tests
  tests/testutils.cpp
  tests/schema_tests.cpp
  tests/moveprocessor_tests.cpp
  tests/statejson_tests.cpp
  tests/dungeon_tests.cpp
  tests/dungeongame_tests.cpp
  tests/playthrough_test.cpp
  moveparser.cpp
  moveprocessor.cpp
  statejson.cpp
  dungeon.cpp
  dungeongame.cpp
  monsters.cpp
  combat.cpp
  items.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/schema_test.cpp
)

target_include_directories (roguelike_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

if (JSONCPP_FOUND)
  target_link_libraries (roguelike_tests PRIVATE PkgConfig::JSONCPP)
else ()
  target_link_libraries (roguelike_tests PRIVATE jsoncpp_lib)
endif ()

target_link_libraries (roguelike_tests PRIVATE
  PkgConfig::SQLITE3
  glog::glog
  GTest::gtest_main
)

add_test (NAME roguelike_tests COMMAND roguelike_tests)

# ============================================================
# Full build (requires libxayagame installed)
# ============================================================

if (XAYAGAME_FOUND AND XAYAUTIL_FOUND)
  message (STATUS "Found installed libxayagame — building full daemon")

  pkg_check_modules (JSONRPCSERVER REQUIRED IMPORTED_TARGET libjsonrpccpp-server)

  # RPC stub generation.
  file (MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rpc-stubs)
  add_custom_command (
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rpc-stubs/rogrpcserverstub.h
    COMMAND ${JSONRPCSTUB} ${CMAKE_CURRENT_SOURCE_DIR}/rpc-stubs/rog.json
      --cpp-server=RogRpcServerStub
      --cpp-server-file=${CMAKE_CURRENT_BINARY_DIR}/rpc-stubs/rogrpcserverstub.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rpc-stubs/rog.json
  )
  add_custom_target (roguelike_rpc_stubs DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/rpc-stubs/rogrpcserverstub.h
  )

  # Roguelike library.
  add_library (roguelike SHARED
    logic.cpp
    moveparser.cpp
    moveprocessor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/schema.cpp
    statejson.cpp
    dungeon.cpp
    dungeongame.cpp
    monsters.cpp
    combat.cpp
    items.cpp
  )
  target_include_directories (roguelike PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  )
  target_link_libraries (roguelike PUBLIC
    PkgConfig::XAYAGAME
    PkgConfig::XAYAUTIL
    PkgConfig::JSONCPP
    PkgConfig::SQLITE3
    glog::glog
  )

  # Daemon binary.
  add_executable (rogueliked
    main.cpp
    pending.cpp
    rpcserver.cpp
  )
  add_dependencies (rogueliked roguelike_rpc_stubs)
  target_include_directories (rogueliked PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries (rogueliked PRIVATE
    roguelike
    PkgConfig::JSONRPCSERVER
    glog::glog
    gflags
  )

  install (TARGETS rogueliked RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else ()
  message (STATUS "libxayagame not installed — building tests only")
endif ()
