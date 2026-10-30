cmake_minimum_required(VERSION 3.20)
project(raindesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(raindesk WIN32 src/main.cpp src/rain_sim.cpp src/sky.cpp src/sound.cpp src/controls.cpp res/raindesk.rc)
target_compile_definitions(raindesk PRIVATE UNICODE _UNICODE)
target_link_libraries(raindesk PRIVATE d3d11 dxgi dcomp d3dcompiler windowscodecs ole32 comctl32 winmm)
if(MSVC)
  target_compile_options(raindesk PRIVATE /W4 /permissive- /utf-8)
endif()

# The shader source is built into the exe as a resource and compiled at
# startup, so the exe runs on its own. Rebuild the resource when it changes.
set_source_files_properties(res/raindesk.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/rain.hlsl)
# A rain.hlsl next to the exe would take the place of the built-in one (that is
# how the look is tuned live), so clear out the copy earlier builds put there.
add_custom_command(TARGET raindesk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE_DIR:raindesk>/rain.hlsl)

# Sign the exe with your code-signing certificate, so you can tell later
# whether it has been modified (sign.bat /check). sign.bat skips signing on a
# machine without the certificate.
option(RAINDESK_SIGN "Sign raindesk.exe after every build" ON)
if(WIN32 AND RAINDESK_SIGN)
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/sign.bat" RAINDESK_SIGN_BAT)
  add_custom_command(TARGET raindesk POST_BUILD
    COMMAND cmd /c call ${RAINDESK_SIGN_BAT} $<SHELL_PATH:$<TARGET_FILE:raindesk>>
    VERBATIM)
endif()
