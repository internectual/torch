cmake_minimum_required(VERSION 3.20)
project(torch VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
pkg_check_modules(OpenAL REQUIRED IMPORTED_TARGET openal)
pkg_check_modules(GLU REQUIRED IMPORTED_TARGET glu)

find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(torch
    src/main.cpp
    src/stb_image_impl.cpp
    src/stb_image_write_impl.cpp
    src/core/engine.cpp
    src/core/platform.cpp
    src/core/console.cpp
    src/core/math.cpp
    src/core/timer.cpp
    src/core/string_table.cpp
    src/fs/file_system.cpp
    src/fs/vol_archive.cpp
    src/fs/vl2_archive.cpp
    src/script/dso_reader.cpp
    src/script/script_engine.cpp
    src/render/renderer.cpp
    src/render/shader.cpp
    src/render/texture.cpp
    src/render/mesh.cpp
    src/render/terrain.cpp
    src/render/font.cpp
    src/render/sky.cpp
    src/render/glb_loader.cpp
    src/audio/audio_system.cpp
    src/net/network.cpp
    src/net/protocol.cpp
    src/net/connection.cpp
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/game.cpp
    src/game/hud.cpp
    src/game/menu.cpp
)

target_include_directories(torch PRIVATE src)

target_link_libraries(torch PRIVATE
    OpenGL::GL
    ZLIB::ZLIB
    PkgConfig::SDL3
    PkgConfig::GLEW
    PkgConfig::OpenAL
    PkgConfig::GLU
    m
    dl
)

target_compile_definitions(torch PRIVATE
    GLEW_STATIC
    T2_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
