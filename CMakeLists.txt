cmake_minimum_required(VERSION 3.20)
project(torch VERSION 0.1.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-missing-field-initializers")

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
pkg_check_modules(OpenAL REQUIRED IMPORTED_TARGET openal)
pkg_check_modules(GLU REQUIRED IMPORTED_TARGET glu)

find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

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
    src/fs/asset_policy.cpp
    src/fs/path_policy.cpp
    src/fs/vol_archive.cpp
    src/fs/vl2_archive.cpp
    src/script/dso_reader.cpp
    src/script/script_engine.cpp
    src/script/torquescript.cpp
    src/render/renderer.cpp
    src/render/shader.cpp
    src/render/texture.cpp
    src/render/mesh.cpp
    src/render/terrain.cpp
    src/render/font.cpp
    src/render/sky.cpp
    src/render/dts_loader.cpp
    src/render/dif_loader.cpp
    src/render/gui_renderer.cpp
    src/audio/audio_system.cpp
    src/net/network.cpp
    src/net/master_query.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_registry.cpp
    src/net/v12_events.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_ghost_packet.cpp
    src/net/protocol.cpp
    src/net/connection.cpp
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/game/hud.cpp
    src/game/menu.cpp
    src/game/demo.cpp
)

target_include_directories(torch PRIVATE src)

target_link_libraries(torch PRIVATE
    OpenGL::GL
    ZLIB::ZLIB
    PkgConfig::SDL3
    PkgConfig::GLEW
    PkgConfig::OpenAL
    PkgConfig::GLU
    CURL::libcurl
    vorbisfile vorbis
    m
    dl
)

target_compile_definitions(torch PRIVATE
    GLEW_STATIC
    TORCH_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TORCH_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TORCH_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Dedicated server (headless, no rendering/audio)
add_executable(torch_server
    src/server_main.cpp
    src/stb_image_impl.cpp
    src/stb_image_write_impl.cpp
    src/core/engine.cpp
    src/core/platform.cpp
    src/core/console.cpp
    src/core/math.cpp
    src/core/timer.cpp
    src/core/string_table.cpp
    src/fs/file_system.cpp
    src/fs/asset_policy.cpp
    src/fs/path_policy.cpp
    src/fs/vol_archive.cpp
    src/fs/vl2_archive.cpp
    src/script/dso_reader.cpp
    src/script/script_engine.cpp
    src/script/torquescript.cpp
    src/render/renderer.cpp
    src/render/shader.cpp
    src/render/texture.cpp
    src/render/mesh.cpp
    src/render/terrain.cpp
    src/render/font.cpp
    src/render/sky.cpp
    src/render/dts_loader.cpp
    src/render/dif_loader.cpp
    src/render/gui_renderer.cpp
    src/audio/audio_system.cpp
    src/net/network.cpp
    src/net/master_query.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_registry.cpp
    src/net/v12_events.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_ghost_packet.cpp
    src/net/protocol.cpp
    src/net/connection.cpp
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/game/hud.cpp
    src/game/menu.cpp
    src/game/demo.cpp
)

target_include_directories(torch_server PRIVATE src)

target_link_libraries(torch_server PRIVATE
    OpenGL::GL
    ZLIB::ZLIB
    PkgConfig::SDL3
    PkgConfig::GLEW
    PkgConfig::OpenAL
    PkgConfig::GLU
    CURL::libcurl
    vorbisfile vorbis
    m
    dl
)

target_compile_definitions(torch_server PRIVATE
    TORCH_DEDICATED
    TORCH_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TORCH_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TORCH_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Install rules
install(TARGETS torch torch_server
    RUNTIME DESTINATION bin)

install(FILES README.md
    DESTINATION share/torch)

# CPack packaging
set(CPACK_PACKAGE_NAME "torch")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Torque2D engine reimplementation")
set(CPACK_GENERATOR "TGZ;DEB")
include(CPack)

add_executable(torch_asset_policy_test
    tests/asset_policy_test.cpp
    src/fs/asset_policy.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_registry.cpp
    src/net/v12_events.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_ghost_packet.cpp
)
target_include_directories(torch_asset_policy_test PRIVATE src)
target_compile_options(torch_asset_policy_test PRIVATE -UNDEBUG)
add_test(NAME torch_asset_policy COMMAND torch_asset_policy_test)

add_executable(torch_transform_test
    tests/transform_test.cpp
    src/core/math.cpp
)
target_include_directories(torch_transform_test PRIVATE src)
target_compile_options(torch_transform_test PRIVATE -UNDEBUG)
add_test(NAME torch_transform COMMAND torch_transform_test)

add_executable(torch_safety_policy_test
    tests/safety_policy_test.cpp
    src/fs/path_policy.cpp
    src/net/master_query.cpp
)
target_include_directories(torch_safety_policy_test PRIVATE src)
add_test(NAME torch_safety_policy COMMAND torch_safety_policy_test)

add_executable(torch_demo_snapshot_test
    tests/demo_snapshot_test.cpp
    src/game/demo.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_registry.cpp
    src/net/v12_events.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_ghost_packet.cpp
)
target_include_directories(torch_demo_snapshot_test PRIVATE src)
target_link_libraries(torch_demo_snapshot_test PRIVATE ZLIB::ZLIB m)
target_compile_options(torch_demo_snapshot_test PRIVATE -UNDEBUG)
add_test(NAME torch_demo_snapshot
    COMMAND torch_demo_snapshot_test
        $ENV{HOME}/.torch/base/recordings/treachery.rec)
