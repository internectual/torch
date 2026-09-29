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
    src/sim/sim_natives.cpp
    src/sim/net_string_table.cpp
    src/sim/engine_object.cpp
    src/sim/game_base.cpp
    src/sim/shape_base.cpp
    src/sim/camera.cpp
    src/sim/player.cpp
    src/sim/static_shapes.cpp
    src/sim/nav_graph.cpp
    src/sim/target_manager.cpp
    src/sim/containers.cpp
    src/sim/projectiles.cpp
    src/sim/server_container.cpp
    src/sim/trigger.cpp
    src/sim/net_interface.cpp
    src/sim/game_connection.cpp
    src/sim/net_object.cpp
    src/sim/scene_objects.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
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
    src/game/world.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
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
    src/sim/sim_natives.cpp
    src/sim/net_string_table.cpp
    src/sim/engine_object.cpp
    src/sim/game_base.cpp
    src/sim/shape_base.cpp
    src/sim/camera.cpp
    src/sim/player.cpp
    src/sim/static_shapes.cpp
    src/sim/nav_graph.cpp
    src/sim/target_manager.cpp
    src/sim/containers.cpp
    src/sim/projectiles.cpp
    src/sim/server_container.cpp
    src/sim/trigger.cpp
    src/sim/net_interface.cpp
    src/sim/game_connection.cpp
    src/sim/net_object.cpp
    src/sim/scene_objects.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
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
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
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

add_executable(torch_collision_bounds_test
    tests/collision_bounds_test.cpp
    src/game/collision.cpp
)
target_include_directories(torch_collision_bounds_test PRIVATE src)
target_compile_options(torch_collision_bounds_test PRIVATE -UNDEBUG)
add_test(NAME torch_collision_bounds COMMAND torch_collision_bounds_test)

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
    src/core/math.cpp
)
target_include_directories(torch_mission_parser_test PRIVATE src)
target_compile_options(torch_mission_parser_test PRIVATE -UNDEBUG)
add_test(NAME torch_mission_parser COMMAND torch_mission_parser_test)

add_executable(torch_mission_discovery_test tests/mission_discovery_test.cpp)
target_include_directories(torch_mission_discovery_test PRIVATE src)
target_compile_options(torch_mission_discovery_test PRIVATE -UNDEBUG)
add_test(NAME torch_mission_discovery COMMAND torch_mission_discovery_test)

add_executable(torch_mission_resolution_test
    tests/mission_resolution_test.cpp
    tests/console_stub.cpp
    src/fs/file_system.cpp
    src/fs/asset_policy.cpp
    src/fs/path_policy.cpp
    src/fs/vl2_archive.cpp
)
target_include_directories(torch_mission_resolution_test PRIVATE src)
target_link_libraries(torch_mission_resolution_test PRIVATE ZLIB::ZLIB)
target_compile_options(torch_mission_resolution_test PRIVATE -UNDEBUG)
add_test(NAME torch_mission_resolution COMMAND torch_mission_resolution_test)

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

add_executable(torch_script_demo_contract_test tests/script_demo_contract_test.cpp)
target_include_directories(torch_script_demo_contract_test PRIVATE src)
target_compile_options(torch_script_demo_contract_test PRIVATE -UNDEBUG)
add_test(NAME torch_script_demo_contract COMMAND torch_script_demo_contract_test)

add_executable(torch_trigger_test
    tests/trigger_test.cpp
    src/core/math.cpp
)
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

add_executable(torch_weapon_spawn_test
    tests/weapon_spawn_test.cpp
    src/game/weapon.cpp
)
target_include_directories(torch_weapon_spawn_test PRIVATE src)
target_compile_options(torch_weapon_spawn_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
target_link_options(torch_weapon_spawn_test PRIVATE -Wl,--gc-sections)
target_link_libraries(torch_weapon_spawn_test PRIVATE m)
add_test(NAME torch_weapon_spawn COMMAND torch_weapon_spawn_test)

add_executable(torch_dynamic_lighting_test tests/dynamic_lighting_test.cpp)
target_include_directories(torch_dynamic_lighting_test PRIVATE src)
target_compile_options(torch_dynamic_lighting_test PRIVATE -UNDEBUG)
add_test(NAME torch_dynamic_lighting COMMAND torch_dynamic_lighting_test)

add_executable(torch_parity_test
    tests/parity_test.cpp
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
    src/sim/sim_natives.cpp
    src/sim/net_string_table.cpp
    src/sim/engine_object.cpp
    src/sim/game_base.cpp
    src/sim/shape_base.cpp
    src/sim/camera.cpp
    src/sim/player.cpp
    src/sim/static_shapes.cpp
    src/sim/nav_graph.cpp
    src/sim/target_manager.cpp
    src/sim/containers.cpp
    src/sim/projectiles.cpp
    src/sim/server_container.cpp
    src/sim/trigger.cpp
    src/sim/net_interface.cpp
    src/sim/game_connection.cpp
    src/sim/net_object.cpp
    src/sim/scene_objects.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
    src/script/torquescript.cpp
    src/render/renderer.cpp
    src/render/shader.cpp
    src/render/texture.cpp
    src/render/mesh.cpp
    src/render/font.cpp
    src/render/sky.cpp
    src/render/dts_loader.cpp
    src/render/dif_loader.cpp
    src/render/gui_renderer.cpp
    src/audio/audio_system.cpp
    src/game/demo.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/net/network.cpp
    src/net/master_query.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_events.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_registry.cpp
    src/net/v12_ghost_packet.cpp
    src/render/terrain_holes.cpp
    src/render/terrain.cpp
)
target_include_directories(torch_parity_test PRIVATE src)
target_link_libraries(torch_parity_test PRIVATE
    OpenGL::GL ZLIB::ZLIB PkgConfig::SDL3 PkgConfig::GLEW PkgConfig::OpenAL
    PkgConfig::GLU CURL::libcurl vorbisfile vorbis m dl)
target_compile_definitions(torch_parity_test PRIVATE GLEW_STATIC)
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
    src/sim/sim_natives.cpp
    src/sim/net_string_table.cpp
    src/sim/engine_object.cpp
    src/sim/game_base.cpp
    src/sim/shape_base.cpp
    src/sim/camera.cpp
    src/sim/player.cpp
    src/sim/static_shapes.cpp
    src/sim/nav_graph.cpp
    src/sim/target_manager.cpp
    src/sim/containers.cpp
    src/sim/projectiles.cpp
    src/sim/server_container.cpp
    src/sim/trigger.cpp
    src/sim/net_interface.cpp
    src/sim/game_connection.cpp
    src/sim/net_object.cpp
    src/sim/scene_objects.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
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
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
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

add_executable(torch_target_manager_test
    tests/target_manager_test.cpp
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
    src/sim/sim_natives.cpp
    src/sim/net_string_table.cpp
    src/sim/engine_object.cpp
    src/sim/game_base.cpp
    src/sim/shape_base.cpp
    src/sim/camera.cpp
    src/sim/player.cpp
    src/sim/static_shapes.cpp
    src/sim/nav_graph.cpp
    src/sim/target_manager.cpp
    src/sim/containers.cpp
    src/sim/projectiles.cpp
    src/sim/server_container.cpp
    src/sim/trigger.cpp
    src/sim/net_interface.cpp
    src/sim/game_connection.cpp
    src/sim/net_object.cpp
    src/sim/scene_objects.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
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
    src/game/world.cpp
    src/game/player.cpp
    src/game/physics.cpp
    src/game/collision.cpp
    src/game/weapon.cpp
    src/game/game.cpp
    src/render/projected_shadows.cpp
    src/game/hud.cpp
    src/game/menu.cpp
    src/game/demo.cpp
)
target_include_directories(torch_target_manager_test PRIVATE src)
target_link_libraries(torch_target_manager_test PRIVATE
    OpenGL::GL ZLIB::ZLIB PkgConfig::SDL3 PkgConfig::GLEW PkgConfig::OpenAL
    PkgConfig::GLU CURL::libcurl vorbisfile vorbis m dl)
target_compile_definitions(torch_target_manager_test PRIVATE GLEW_STATIC)
target_compile_options(torch_target_manager_test PRIVATE -UNDEBUG)
add_test(NAME torch_target_manager COMMAND torch_target_manager_test)
set_tests_properties(torch_target_manager PROPERTIES TIMEOUT 10)

add_executable(torch_demo_snapshot_test
    tests/demo_snapshot_test.cpp
    src/game/demo.cpp
    src/core/math.cpp
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

add_executable(torch_torque_bit_writer_test
    tests/torque_bit_writer_test.cpp
    tests/console_stub.cpp
    src/game/demo.cpp
    src/core/math.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_protocol.cpp
    src/net/v12_registry.cpp
    src/net/v12_events.cpp
    src/net/v12_datablocks.cpp
    src/net/v12_ghosts.cpp
    src/net/v12_ghost_packet.cpp
)
target_include_directories(torch_torque_bit_writer_test PRIVATE src)
target_link_libraries(torch_torque_bit_writer_test PRIVATE ZLIB::ZLIB m)
target_compile_options(torch_torque_bit_writer_test PRIVATE -UNDEBUG)
add_test(NAME torch_torque_bit_writer COMMAND torch_torque_bit_writer_test)

add_executable(torch_datablock_pack_shapes_test
    tests/datablock_pack_shapes_test.cpp
    tests/console_stub.cpp
    tests/vmvalue_stub.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_registry.cpp
    src/net/v12_datablocks.cpp
)
target_include_directories(torch_datablock_pack_shapes_test PRIVATE src .)
target_compile_options(torch_datablock_pack_shapes_test PRIVATE -UNDEBUG)
add_test(NAME torch_datablock_pack_shapes COMMAND torch_datablock_pack_shapes_test)

add_executable(torch_datablock_pack_projectiles_test
    tests/datablock_pack_projectiles_test.cpp
    tests/console_stub.cpp
    tests/vmvalue_stub.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_registry.cpp
    src/net/v12_datablocks.cpp
)
target_include_directories(torch_datablock_pack_projectiles_test PRIVATE src .)
target_compile_options(torch_datablock_pack_projectiles_test PRIVATE -UNDEBUG)
add_test(NAME torch_datablock_pack_projectiles COMMAND torch_datablock_pack_projectiles_test)

add_executable(torch_datablock_pack_effects_test
    tests/datablock_pack_effects_test.cpp
    tests/console_stub.cpp
    tests/vmvalue_stub.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_registry.cpp
    src/net/v12_datablocks.cpp
)
target_include_directories(torch_datablock_pack_effects_test PRIVATE src .)
target_compile_options(torch_datablock_pack_effects_test PRIVATE -UNDEBUG)
add_test(NAME torch_datablock_pack_effects COMMAND torch_datablock_pack_effects_test)

add_executable(torch_datablock_pack_misc_test
    tests/datablock_pack_misc_test.cpp
    tests/console_stub.cpp
    tests/vmvalue_stub.cpp
    src/sim/datablock_pack.cpp
    src/sim/datablocks/shapes.cpp
    src/sim/datablocks/projectiles.cpp
    src/sim/datablocks/effects.cpp
    src/sim/datablocks/misc.cpp
    src/net/v12_bitstream.cpp
    src/net/v12_registry.cpp
    src/net/v12_datablocks.cpp
)
target_include_directories(torch_datablock_pack_misc_test PRIVATE src .)
target_compile_options(torch_datablock_pack_misc_test PRIVATE -UNDEBUG)
add_test(NAME torch_datablock_pack_misc COMMAND torch_datablock_pack_misc_test)

add_executable(torch_demo_mission_replacement_test
    tests/demo_mission_replacement_test.cpp
)
target_include_directories(torch_demo_mission_replacement_test PRIVATE src)
target_compile_options(torch_demo_mission_replacement_test PRIVATE -UNDEBUG)
add_test(NAME torch_demo_mission_replacement COMMAND torch_demo_mission_replacement_test)

add_executable(torch_demo_text_test tests/demo_text_test.cpp)
target_include_directories(torch_demo_text_test PRIVATE src)
target_compile_options(torch_demo_text_test PRIVATE -UNDEBUG)
add_test(NAME torch_demo_text COMMAND torch_demo_text_test)

add_executable(torch_weapon_image_state_test tests/weapon_image_state_test.cpp)
target_include_directories(torch_weapon_image_state_test PRIVATE src)
target_compile_options(torch_weapon_image_state_test PRIVATE -UNDEBUG)
add_test(NAME torch_weapon_image_state COMMAND torch_weapon_image_state_test)

add_executable(torch_vehicle_jets_test tests/vehicle_jets_test.cpp)
target_include_directories(torch_vehicle_jets_test PRIVATE src)
target_compile_options(torch_vehicle_jets_test PRIVATE -UNDEBUG)
add_test(NAME torch_vehicle_jets COMMAND torch_vehicle_jets_test)

add_executable(torch_projectile_physics_test tests/projectile_physics_test.cpp)
target_include_directories(torch_projectile_physics_test PRIVATE src)
target_compile_options(torch_projectile_physics_test PRIVATE -UNDEBUG)
add_test(NAME torch_projectile_physics COMMAND torch_projectile_physics_test)

add_executable(torch_flare_spikes_test tests/flare_spikes_test.cpp)
target_include_directories(torch_flare_spikes_test PRIVATE src)
target_compile_options(torch_flare_spikes_test PRIVATE -UNDEBUG)
add_test(NAME torch_flare_spikes COMMAND torch_flare_spikes_test)

add_executable(torch_shape_lighting_test tests/shape_lighting_test.cpp)
target_include_directories(torch_shape_lighting_test PRIVATE src)
target_compile_options(torch_shape_lighting_test PRIVATE -UNDEBUG)
add_test(NAME torch_shape_lighting COMMAND torch_shape_lighting_test)

add_executable(torch_player_animation_test tests/player_animation_test.cpp)
target_include_directories(torch_player_animation_test PRIVATE src)
target_compile_options(torch_player_animation_test PRIVATE -UNDEBUG)
add_test(NAME torch_player_animation COMMAND torch_player_animation_test)

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

add_executable(torch_dso_reader_test
    tests/dso_reader_test.cpp
    tests/console_stub.cpp
    src/script/dso_reader.cpp
)
target_include_directories(torch_dso_reader_test PRIVATE src)
target_compile_options(torch_dso_reader_test PRIVATE -UNDEBUG)
add_test(NAME torch_dso_reader COMMAND torch_dso_reader_test)

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

add_executable(torch_precipitation_parity_test tests/precipitation_parity_test.cpp)
target_include_directories(torch_precipitation_parity_test PRIVATE src)
target_compile_options(torch_precipitation_parity_test PRIVATE -UNDEBUG)
add_test(NAME torch_precipitation_parity COMMAND torch_precipitation_parity_test)

add_executable(torch_sky_parity_test tests/sky_parity_test.cpp)
target_include_directories(torch_sky_parity_test PRIVATE src)
target_compile_options(torch_sky_parity_test PRIVATE -UNDEBUG)
add_test(NAME torch_sky_parity COMMAND torch_sky_parity_test)

add_executable(torch_text_layout_test tests/text_layout_test.cpp)
target_include_directories(torch_text_layout_test PRIVATE src)
target_compile_options(torch_text_layout_test PRIVATE -UNDEBUG)
add_test(NAME torch_text_layout COMMAND torch_text_layout_test)

add_executable(torch_script_conversion_test tests/script_conversion_test.cpp)
target_include_directories(torch_script_conversion_test PRIVATE src)
target_compile_options(torch_script_conversion_test PRIVATE -UNDEBUG)
add_test(NAME torch_script_conversion COMMAND torch_script_conversion_test)

add_executable(torch_menu_parity_test tests/menu_parity_test.cpp)
target_include_directories(torch_menu_parity_test PRIVATE src)
target_compile_options(torch_menu_parity_test PRIVATE -UNDEBUG)
add_test(NAME torch_menu_parity COMMAND torch_menu_parity_test)

add_executable(torch_item_visibility_test tests/item_visibility_test.cpp)
target_include_directories(torch_item_visibility_test PRIVATE src)
target_compile_options(torch_item_visibility_test PRIVATE -UNDEBUG)
add_test(NAME torch_item_visibility COMMAND torch_item_visibility_test)

add_test(NAME torch_runtime_lifecycle
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_lifecycle_test.sh
        $<TARGET_FILE:torch> $<TARGET_FILE:torch_server>)
set_tests_properties(torch_runtime_lifecycle PROPERTIES TIMEOUT 90)

add_test(NAME torch_launch_matrix
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/launch_matrix_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_launch_matrix PROPERTIES TIMEOUT 120)

add_test(NAME torch_help_output
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/help_output_test.sh
        $<TARGET_FILE:torch> $<TARGET_FILE:torch_server>)

add_test(NAME torch_script_path_precedence
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_path_precedence_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_path_precedence PROPERTIES TIMEOUT 45)

add_test(NAME torch_script_dependency
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_dependency_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_dependency PROPERTIES TIMEOUT 60)

add_test(NAME torch_script_compile_failure
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_compile_failure_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_compile_failure PROPERTIES TIMEOUT 45)

add_test(NAME torch_script_dso_roundtrip
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_dso_roundtrip_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_dso_roundtrip PROPERTIES TIMEOUT 45)

add_test(NAME torch_mouse_device
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/mouse_device_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_mouse_device PROPERTIES TIMEOUT 45)

add_test(NAME torch_script_word_range
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_word_range_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_word_range PROPERTIES TIMEOUT 45)
add_test(NAME torch_script_object_semantics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/script_object_semantics_test.sh
        $<TARGET_FILE:torch>)
set_tests_properties(torch_script_object_semantics PROPERTIES TIMEOUT 45)

add_executable(torch_shadow_projection_test tests/shadow_projection_test.cpp src/core/math.cpp)
target_include_directories(torch_shadow_projection_test PRIVATE src)
target_compile_options(torch_shadow_projection_test PRIVATE -UNDEBUG)
add_test(NAME torch_shadow_projection COMMAND torch_shadow_projection_test)

add_executable(torch_timeline_random_test tests/timeline_random_test.cpp)
target_include_directories(torch_timeline_random_test PRIVATE src)
target_compile_options(torch_timeline_random_test PRIVATE -UNDEBUG)
add_test(NAME torch_timeline_random COMMAND torch_timeline_random_test)

add_executable(torch_shape_threads_test tests/shape_threads_test.cpp)
target_include_directories(torch_shape_threads_test PRIVATE src)
target_compile_options(torch_shape_threads_test PRIVATE -UNDEBUG)
add_test(NAME torch_shape_threads COMMAND torch_shape_threads_test)

add_executable(torch_demo_camera_test tests/demo_camera_test.cpp)
target_include_directories(torch_demo_camera_test PRIVATE src)
target_compile_options(torch_demo_camera_test PRIVATE -UNDEBUG)
add_test(NAME torch_demo_camera COMMAND torch_demo_camera_test)

add_executable(torch_item_physics_test tests/item_physics_test.cpp)
target_include_directories(torch_item_physics_test PRIVATE src)
target_compile_options(torch_item_physics_test PRIVATE -UNDEBUG)
add_test(NAME torch_item_physics COMMAND torch_item_physics_test)

add_executable(torch_fluid_test tests/fluid_test.cpp)
target_include_directories(torch_fluid_test PRIVATE src)
target_compile_options(torch_fluid_test PRIVATE -UNDEBUG)
add_test(NAME torch_fluid COMMAND torch_fluid_test)

add_executable(torch_player_prediction_test tests/player_prediction_test.cpp)
target_include_directories(torch_player_prediction_test PRIVATE src)
target_compile_options(torch_player_prediction_test PRIVATE -UNDEBUG)
add_test(NAME torch_player_prediction COMMAND torch_player_prediction_test)

add_executable(torch_dts_triggers_test tests/dts_triggers_test.cpp)
target_include_directories(torch_dts_triggers_test PRIVATE src)
target_link_libraries(torch_dts_triggers_test PRIVATE PkgConfig::GLEW OpenGL::GL)
target_compile_options(torch_dts_triggers_test PRIVATE -UNDEBUG)
add_test(NAME torch_dts_triggers COMMAND torch_dts_triggers_test)

add_executable(torch_remote_command_test tests/remote_command_test.cpp)
target_include_directories(torch_remote_command_test PRIVATE src)
target_compile_options(torch_remote_command_test PRIVATE -UNDEBUG)
add_test(NAME torch_remote_command COMMAND torch_remote_command_test)
