cmake_minimum_required(VERSION 3.20)
project(toggleHDR LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "toggleHDR requires Windows or a Windows cross-toolchain.")
endif()

add_executable(toggleHDR WIN32
    toggleHDR/toggleHDR.cpp
    toggleHDR/Display.cpp
    toggleHDR/SettingsWindow.cpp
    toggleHDR/NativeHdr.h
    toggleHDR/resource.h
    toggleHDR/Resources.rc
)
target_compile_features(toggleHDR PRIVATE cxx_std_17)
target_include_directories(toggleHDR PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/toggleHDR")
target_compile_definitions(toggleHDR PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(toggleHDR PRIVATE user32 shell32 gdi32 ole32 advapi32)

if(MSVC)
    target_compile_options(toggleHDR PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET toggleHDR PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(toggleHDR PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_compile_options(toggleHDR PRIVATE -Wall -Wextra)
    target_link_options(toggleHDR PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS toggleHDR RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)
