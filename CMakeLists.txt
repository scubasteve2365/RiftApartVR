cmake_minimum_required(VERSION 3.20)
project(RiftApartVR LANGUAGES C CXX ASM_MASM)

# 64-bit only, because the game is 64-bit:
#   cmake -S . -B ..\build\cmake -A x64
#   cmake --build ..\build\cmake --config Release
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure with -A x64. The game is 64-bit.")
endif()

add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(winhttp SHARED src/main.cpp src/capture.cpp src/xr.cpp src/winhttp_stubs.asm src/winhttp.def)
target_compile_features(winhttp PRIVATE cxx_std_17)
target_compile_definitions(winhttp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(winhttp PRIVATE minhook)
set(OPENXR_DIR "C:/VRLab/bin/openxr_loader_1.1.65")
target_include_directories(winhttp PRIVATE third_party/minhook/include ${OPENXR_DIR}/include)
set_property(TARGET winhttp minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_custom_command(TARGET winhttp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:winhttp> ${CMAKE_CURRENT_SOURCE_DIR}/../build/winhttp.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENXR_DIR}/x64/bin/openxr_loader.dll ${CMAKE_CURRENT_SOURCE_DIR}/../build/openxr_loader.dll)
