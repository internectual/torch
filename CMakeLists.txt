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
    src/render/terrain_holes.cpp
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
    src/net/v12_events.cpp
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
    src/render/terrain_holes.cpp
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

add_executable(torch_script_object_state_test tests/script_object_state_test.cpp)
target_include_directories(torch_script_object_state_test PRIVATE src)
target_compile_options(torch_script_object_state_test PRIVATE -UNDEBUG)
add_test(NAME torch_script_object_state COMMAND torch_script_object_state_test)

add_executable(torch_safety_policy_test
    tests/safety_policy_test.cpp
    src/fs/path_policy.cpp
    src/net/master_query.cpp
)
target_include_directories(torch_safety_policy_test PRIVATE src)
add_test(NAME torch_safety_policy COMMAND torch_safety_policy_test)

add_executable(torch_mission_parser_test
    tests/mission_parser_test.cpp
)
target_include_directories(torch_mission_parser_test PRIVATE src)
target_compile_options(torch_mission_parser_test PRIVATE -UNDEBUG)
add_test(NAME torch_mission_parser COMMAND torch_mission_parser_test)

add_executable(torch_mission_discovery_test tests/mission_discovery_test.cpp)
target_include_directories(torch_mission_discovery_test PRIVATE src)
target_compile_options(torch_mission_discovery_test PRIVATE -UNDEBUG)
add_test(NAME torch_mission_discovery COMMAND torch_mission_discovery_test)

add_executable(torch_script_scheduler_test tests/script_scheduler_test.cpp)
target_include_directories(torch_script_scheduler_test PRIVATE src)
target_compile_options(torch_script_scheduler_test PRIVATE -UNDEBUG)
add_test(NAME torch_script_scheduler COMMAND torch_script_scheduler_test)

add_executable(torch_dialog_lifecycle_test tests/dialog_lifecycle_test.cpp)
target_include_directories(torch_dialog_lifecycle_test PRIVATE src)
target_compile_options(torch_dialog_lifecycle_test PRIVATE -UNDEBUG)
add_test(NAME torch_dialog_lifecycle COMMAND torch_dialog_lifecycle_test)

add_executable(torch_gui_input_test tests/gui_input_test.cpp)
target_include_directories(torch_gui_input_test PRIVATE src)
target_compile_options(torch_gui_input_test PRIVATE -UNDEBUG)
add_test(NAME torch_gui_input COMMAND torch_gui_input_test)

add_executable(torch_gui_geometry_test tests/gui_geometry_test.cpp)
target_include_directories(torch_gui_geometry_test PRIVATE src)
target_compile_options(torch_gui_geometry_test PRIVATE -UNDEBUG)
add_test(NAME torch_gui_geometry COMMAND torch_gui_geometry_test)

add_executable(torch_console_args_test tests/console_args_test.cpp)
target_include_directories(torch_console_args_test PRIVATE src)
target_compile_options(torch_console_args_test PRIVATE -UNDEBUG)
add_test(NAME torch_console_args COMMAND torch_console_args_test)

add_executable(torch_launch_args_test tests/launch_args_test.cpp)
target_include_directories(torch_launch_args_test PRIVATE src)
target_compile_options(torch_launch_args_test PRIVATE -UNDEBUG)
add_test(NAME torch_launch_args COMMAND torch_launch_args_test)

add_executable(torch_trigger_test tests/trigger_test.cpp)
target_include_directories(torch_trigger_test PRIVATE src)
target_compile_options(torch_trigger_test PRIVATE -UNDEBUG)
add_test(NAME torch_trigger COMMAND torch_trigger_test)

add_executable(torch_underwater_audio_test
    tests/underwater_audio_test.cpp
)
target_include_directories(torch_underwater_audio_test PRIVATE src)
target_compile_options(torch_underwater_audio_test PRIVATE -UNDEBUG)
add_test(NAME torch_underwater_audio COMMAND torch_underwater_audio_test)

add_executable(torch_audio_environment_test
    tests/audio_environment_test.cpp
)
target_include_directories(torch_audio_environment_test PRIVATE src)
target_compile_options(torch_audio_environment_test PRIVATE -UNDEBUG)
add_test(NAME torch_audio_environment COMMAND torch_audio_environment_test)

add_executable(torch_audio_resource_test tests/audio_resource_test.cpp)
target_include_directories(torch_audio_resource_test PRIVATE src)
target_compile_options(torch_audio_resource_test PRIVATE -UNDEBUG)
add_test(NAME torch_audio_resource COMMAND torch_audio_resource_test)

add_executable(torch_time_scale_test tests/time_scale_test.cpp)
target_include_directories(torch_time_scale_test PRIVATE src)
target_compile_options(torch_time_scale_test PRIVATE -UNDEBUG)
add_test(NAME torch_time_scale COMMAND torch_time_scale_test)

add_executable(torch_projectile_audio_test tests/projectile_audio_test.cpp)
target_include_directories(torch_projectile_audio_test PRIVATE src)
target_compile_options(torch_projectile_audio_test PRIVATE -UNDEBUG)
add_test(NAME torch_projectile_audio COMMAND torch_projectile_audio_test)

add_executable(torch_projectile_material_test tests/projectile_material_test.cpp)
target_include_directories(torch_projectile_material_test PRIVATE src)
target_compile_options(torch_projectile_material_test PRIVATE -UNDEBUG)
add_test(NAME torch_projectile_material COMMAND torch_projectile_material_test)

add_executable(torch_dynamic_lighting_test tests/dynamic_lighting_test.cpp)
target_include_directories(torch_dynamic_lighting_test PRIVATE src)
target_compile_options(torch_dynamic_lighting_test PRIVATE -UNDEBUG)
add_test(NAME torch_dynamic_lighting COMMAND torch_dynamic_lighting_test)

add_executable(torch_parity_test
    tests/parity_test.cpp
    src/game/demo.cpp
    src/game/collision.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_events.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_registry.cpp
    src/render/terrain_holes.cpp
    src/render/terrain.cpp
)
target_include_directories(torch_parity_test PRIVATE src)
target_link_libraries(torch_parity_test PRIVATE ZLIB::ZLIB m)
target_compile_options(torch_parity_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
target_link_options(torch_parity_test PRIVATE -Wl,--gc-sections)
add_test(NAME torch_parity COMMAND torch_parity_test)

add_executable(torch_network_loopback_test
    tests/network_loopback_test.cpp
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
    src/render/terrain_holes.cpp
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
target_include_directories(torch_network_loopback_test PRIVATE src)
target_link_libraries(torch_network_loopback_test PRIVATE
    OpenGL::GL ZLIB::ZLIB PkgConfig::SDL3 PkgConfig::GLEW PkgConfig::OpenAL
    PkgConfig::GLU CURL::libcurl vorbisfile vorbis m dl)
target_compile_definitions(torch_network_loopback_test PRIVATE GLEW_STATIC)
target_compile_options(torch_network_loopback_test PRIVATE -UNDEBUG)
add_test(NAME torch_network_loopback COMMAND torch_network_loopback_test)
set_tests_properties(torch_network_loopback PROPERTIES TIMEOUT 10)

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
add_test(NAME torch_demo_minotaur
    COMMAND torch_demo_snapshot_test
        $ENV{HOME}/.torch/base/recordings/TWL_Minotaur.rec)
add_test(NAME torch_demo_damnation
    COMMAND torch_demo_snapshot_test
        $ENV{HOME}/.torch/base/recordings/lonzo_damnation.rec)
set_tests_properties(torch_demo_snapshot torch_demo_minotaur torch_demo_damnation
    PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP:")

add_executable(torch_dts_animation_test
    tests/dts_animation_test.cpp
    src/core/math.cpp
)
target_include_directories(torch_dts_animation_test PRIVATE src)
target_compile_options(torch_dts_animation_test PRIVATE -UNDEBUG)
add_test(NAME torch_dts_animation COMMAND torch_dts_animation_test)

add_executable(torch_fog_math_test tests/fog_math_test.cpp)
target_include_directories(torch_fog_math_test PRIVATE src)
target_compile_options(torch_fog_math_test PRIVATE -UNDEBUG)
add_test(NAME torch_fog_math COMMAND torch_fog_math_test)

add_executable(torch_environment_commands_test tests/environment_commands_test.cpp)
target_include_directories(torch_environment_commands_test PRIVATE src)
target_compile_options(torch_environment_commands_test PRIVATE -UNDEBUG)
add_test(NAME torch_environment_commands COMMAND torch_environment_commands_test)

add_executable(torch_filesystem_precedence_test
    tests/filesystem_precedence_test.cpp
    tests/console_stub.cpp
    src/fs/file_system.cpp
    src/fs/asset_policy.cpp
    src/fs/path_policy.cpp
    src/fs/vl2_archive.cpp
)
target_include_directories(torch_filesystem_precedence_test PRIVATE src)
target_link_libraries(torch_filesystem_precedence_test PRIVATE ZLIB::ZLIB)
target_compile_options(torch_filesystem_precedence_test PRIVATE -UNDEBUG)
add_test(NAME torch_filesystem_precedence COMMAND torch_filesystem_precedence_test)

add_executable(torch_decal_runtime_test tests/decal_runtime_test.cpp)
target_include_directories(torch_decal_runtime_test PRIVATE src)
target_compile_options(torch_decal_runtime_test PRIVATE -UNDEBUG)
add_test(NAME torch_decal_runtime COMMAND torch_decal_runtime_test)

add_executable(torch_render_order_test tests/render_order_test.cpp)
target_include_directories(torch_render_order_test PRIVATE src)
target_compile_options(torch_render_order_test PRIVATE -UNDEBUG)
add_test(NAME torch_render_order COMMAND torch_render_order_test)

add_executable(torch_wind_test tests/wind_test.cpp)
target_include_directories(torch_wind_test PRIVATE src)
target_compile_options(torch_wind_test PRIVATE -UNDEBUG)
add_test(NAME torch_wind COMMAND torch_wind_test)

add_executable(torch_text_layout_test tests/text_layout_test.cpp)
target_include_directories(torch_text_layout_test PRIVATE src)
target_compile_options(torch_text_layout_test PRIVATE -UNDEBUG)
add_test(NAME torch_text_layout COMMAND torch_text_layout_test)
