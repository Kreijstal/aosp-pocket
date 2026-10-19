cmake_minimum_required(VERSION 3.22)
project(pocket_runtime LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(HOST_CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "Host emugen compiler")
set(USE_EGL_GETPROCADDRESS 1)
set(BUILD_ANANBOX_DEMO OFF CACHE BOOL "" FORCE)
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.83 REQUIRED COMPONENTS filesystem log_setup log serialization system thread program_options)
get_filename_component(ANBOX "${CMAKE_CURRENT_LIST_DIR}/../../vendor/anbox" ABSOLUTE)
add_subdirectory("${ANBOX}" anbox)
# Override the legacy stream-handshake parser without modifying pinned sources.
set_source_files_properties("${ANBOX}/src/anbox/qemu/adb_message_processor.cpp"
  TARGET_DIRECTORY anbox-core PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(anbox-core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/adb_message_processor.cpp")
set_source_files_properties(
  "${ANBOX}/src/anbox/graphics/emugl/RenderThread.cpp"
  "${ANBOX}/src/anbox/graphics/emugl/Renderer.cpp"
  "${ANBOX}/src/anbox/graphics/buffered_io_stream.cpp"
  "${ANBOX}/src/anbox/input/device.cpp"
  TARGET_DIRECTORY anbox-core PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(anbox-core PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/RenderThread.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/Renderer.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/Device.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/buffered_io_stream.cpp")
target_include_directories(anbox-core BEFORE PUBLIC "${CMAKE_CURRENT_LIST_DIR}/overrides")
add_library(pocket_runtime SHARED runtime.cpp frame_present.cpp)
target_include_directories(pocket_runtime BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/overrides")
target_include_directories(pocket_runtime PRIVATE
  "${ANBOX}" "${ANBOX}/src" "${ANBOX}/external"
  "${ANBOX}/external/android-emugl/shared"
  "${ANBOX}/external/android-emugl/host/include"
  "${ANBOX}/external/android-emugl/host/libs"
  "${ANBOX}/external/android-emugl/shared/OpenglCodecCommon"
  "${ANBOX}/external/android-emugl/host/include/libOpenglRender"
  "${ANBOX}/external/android-emugl/host/libs/GLESv1_dec"
  "${ANBOX}/external/android-emugl/host/libs/GLESv2_dec"
  "${ANBOX}/external/android-emugl/host/libs/renderControl_dec"
  "${CMAKE_BINARY_DIR}/anbox/external/android-emugl/host/include"
  "${CMAKE_BINARY_DIR}/anbox/external/android-emugl/host/libs/GLESv1_dec"
  "${CMAKE_BINARY_DIR}/anbox/external/android-emugl/host/libs/GLESv2_dec"
  "${CMAKE_BINARY_DIR}/anbox/external/android-emugl/host/libs/renderControl_dec"
  ${Boost_INCLUDE_DIRS})
target_link_libraries(pocket_runtime PRIVATE anbox-core android log)
target_link_options(pocket_runtime PRIVATE "-Wl,-z,max-page-size=16384")
