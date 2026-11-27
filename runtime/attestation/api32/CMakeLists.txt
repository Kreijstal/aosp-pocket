# SPDX-License-Identifier: GPL-3.0-only
# Pocket API 32 attestation identity library.
#
# Built as a plain Android NDK shared library and shipped in the API 32 image as
# system/lib64/libpocket_teesim_api32.so. The Manager adds it to the
# pocket-keymaster service's LD_PRELOAD only when the owner selects the
# "teesimulator" backend and an owner-only pocket-teesim.json exists
# (see AttestationPreload32.java).
#
# It deliberately links nothing but libc: the interposed symbols are resolved by
# the loader before libpuresoftkeymasterdevice.so, and the library must work in
# the HAL process even if every optional dependency is missing.
cmake_minimum_required(VERSION 3.22)
project(pocket_teesim_api32 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT ANDROID OR NOT ANDROID_PLATFORM STREQUAL "android-32")
  message(FATAL_ERROR "This adapter targets the Android 12L guest (android-32)")
endif()

add_library(pocket_teesim_api32 SHARED boot_context.cpp)
target_compile_options(pocket_teesim_api32 PRIVATE -fno-rtti -fno-exceptions -Wall -Wextra -Werror)
target_link_options(pocket_teesim_api32 PRIVATE
  # 16 KiB pages: the guest runtime is built for the host kernel's page size.
  -Wl,-z,max-page-size=16384
  # Keep the interposed names visible and nothing else.
  -Wl,--exclude-libs,ALL)
set_target_properties(pocket_teesim_api32 PROPERTIES
  OUTPUT_NAME pocket_teesim_api32
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
