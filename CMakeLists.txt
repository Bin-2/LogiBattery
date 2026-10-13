cmake_minimum_required(VERSION 3.20)
project(LogiBattery VERSION 0.5.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Use MSVC and the Windows SDK for x64.")
endif()
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Configuration" FORCE)
elseif(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Only Release is supported.")
else()
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
configure_file(src/build_config.h.in generated/build_config.h @ONLY)
configure_file(resources/app.manifest.in generated/app.manifest @ONLY)

add_executable(LogiBattery WIN32
    src/main.cpp src/battery.cpp src/battery.h
    src/hid_reader.cpp src/hid_reader.h src/tray_view.cpp src/tray_view.h
    resources/app.rc resources/battery.ico)
target_include_directories(LogiBattery PRIVATE src resources "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(LogiBattery PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(LogiBattery PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(LogiBattery PRIVATE hid setupapi cfgmgr32 shell32 user32 gdi32 advapi32)
target_link_options(LogiBattery PRIVATE /MANIFEST:EMBED
    "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/generated/app.manifest")
set_property(TARGET LogiBattery APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/app.manifest")
install(TARGETS LogiBattery RUNTIME DESTINATION .)
