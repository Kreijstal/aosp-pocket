# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.22)
project(pocket_teesim_api28 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT ANDROID OR NOT ANDROID_PLATFORM STREQUAL "android-28")
  message(FATAL_ERROR "This adapter targets the Android 9 guest (android-28)")
endif()
foreach(required TEESIM_SOURCE API28_SOURCE GUEST_LIBS RUST_TA)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
set(lsplt ${TEESIM_SOURCE}/third_party/lsplt/lsplt/src/main/jni)
add_library(pocket_lsplt STATIC ${lsplt}/lsplt.cc ${lsplt}/elf_util.cc)
target_include_directories(pocket_lsplt PUBLIC ${lsplt}/include PRIVATE ${lsplt})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${TEESIM_SOURCE}/third_party/boringssl ${CMAKE_BINARY_DIR}/boringssl EXCLUDE_FROM_ALL)
add_library(pocket_teesim_api28 SHARED
  ${TEESIM_SOURCE}/keystore/binder_interceptor.cpp
  ${TEESIM_SOURCE}/keystore/keystore_router.cpp
  ${TEESIM_SOURCE}/keystore/keystore_entry.cpp
  ${TEESIM_SOURCE}/common/control.cpp
  ${TEESIM_SOURCE}/common/json.cpp
  ${TEESIM_SOURCE}/common/log_context.cpp
  ${TEESIM_SOURCE}/common/km_names.cpp
  ${TEESIM_SOURCE}/common/redact.cpp
  preload.cpp)
# Use the actual Android 9 Parcel/BBinder layouts, not upstream's newer stubs.
target_include_directories(pocket_teesim_api28 PRIVATE
  ${API28_SOURCE}/frameworks-native/libs/binder/include
  ${API28_SOURCE}/system-core/libutils/include
  ${API28_SOURCE}/system-core/libcutils/include
  ${API28_SOURCE}/system-core/libsystem/include
  ${API28_SOURCE}/system-core/liblog/include
  ${API28_SOURCE}/system-core/base/include
  ${TEESIM_SOURCE}/keystore ${TEESIM_SOURCE}/common
  ${TEESIM_SOURCE}/injector/include ${TEESIM_SOURCE}/rust/teesim-km/include)
# Android 9 log_read.h temporarily undefines __unused; parse modern fortified
# fcntl declarations first, without weakening the NDK's fortify checks.
target_compile_options(pocket_teesim_api28 PRIVATE -include fcntl.h -fno-rtti -fno-exceptions -Wall -Wextra)
target_link_libraries(pocket_teesim_api28 PRIVATE pocket_lsplt ${RUST_TA} crypto
  ${GUEST_LIBS}/libbinder.so ${GUEST_LIBS}/libutils.so log dl)
target_link_options(pocket_teesim_api28 PRIVATE
  -Wl,--no-undefined -Wl,-rpath-link,${GUEST_LIBS}
  -Wl,-Bsymbolic-functions -Wl,--exclude-libs,ALL
  -Wl,-z,max-page-size=16384
  -Wl,--version-script=${TEESIM_SOURCE}/keymint/exports.map)

add_executable(pocket_teesim_wire_test wire_test.cpp
  ${TEESIM_SOURCE}/common/log_context.cpp ${TEESIM_SOURCE}/common/km_names.cpp
  ${TEESIM_SOURCE}/common/redact.cpp)
get_target_property(adapter_includes pocket_teesim_api28 INCLUDE_DIRECTORIES)
target_include_directories(pocket_teesim_wire_test PRIVATE ${adapter_includes})
target_compile_options(pocket_teesim_wire_test PRIVATE -include fcntl.h -fno-rtti -fno-exceptions -Wall -Wextra)
target_link_libraries(pocket_teesim_wire_test PRIVATE ${RUST_TA} crypto
  ${GUEST_LIBS}/libbinder.so ${GUEST_LIBS}/libutils.so log dl)
target_link_options(pocket_teesim_wire_test PRIVATE
  -Wl,--no-undefined -Wl,-rpath-link,${GUEST_LIBS} -Wl,-Bsymbolic-functions -Wl,--exclude-libs,ALL)
if(DEFINED GUEST_LINKER)
  # Local validation only: let qemu-user run the real pinned Android 9 loader.
  target_link_options(pocket_teesim_wire_test PRIVATE -Wl,--dynamic-linker=${GUEST_LINKER})
endif()
