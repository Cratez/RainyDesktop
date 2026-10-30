cmake_minimum_required(VERSION 3.20)
project(raindesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(raindesk WIN32 src/main.cpp src/rain_sim.cpp)
target_compile_definitions(raindesk PRIVATE UNICODE _UNICODE)
target_link_libraries(raindesk PRIVATE d3d11 dxgi dcomp d3dcompiler windowscodecs ole32)
if(MSVC)
  target_compile_options(raindesk PRIVATE /W4 /permissive- /utf-8)
endif()

# The shader is compiled at runtime (and live-reloaded), so it ships next to the exe.
add_custom_command(TARGET raindesk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/shaders/rain.hlsl $<TARGET_FILE_DIR:raindesk>/rain.hlsl)
