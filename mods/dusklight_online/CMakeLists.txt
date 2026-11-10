cmake_minimum_required(VERSION 3.26)
project(dusklight_online LANGUAGES C CXX)
find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

# Distribution builds must not expose the local Windows account or workspace
# through compiler diagnostics, __FILE__ strings, CodeView records, or linked
# static dependencies. MSVC only honours /pathmap consistently when its
# deterministic front-end mode is enabled. Apply this directory-wide before
# declaring the SDK and third-party targets so their linked objects are clean
# as well as the mod's own objects.
if (MSVC)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" DUSKLIGHT_ONLINE_BUILD_USER_ROOT)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" DUSKLIGHT_ONLINE_BUILD_SOURCE_ROOT)
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" DUSKLIGHT_ONLINE_BUILD_BINARY_ROOT)
    add_compile_options(
        "/Zc:__cplusplus"
        "/experimental:deterministic"
        "/pathmap:${DUSKLIGHT_ONLINE_BUILD_SOURCE_ROOT}=Z:\\src"
        "/pathmap:${DUSKLIGHT_ONLINE_BUILD_BINARY_ROOT}=Z:\\build"
    )
    if (DUSKLIGHT_ONLINE_BUILD_USER_ROOT)
        add_compile_options(
            "/pathmap:${DUSKLIGHT_ONLINE_BUILD_USER_ROOT}=Z:\\redacted"
        )
    endif ()
    add_link_options("/Brepro")
endif ()

if (PROJECT_IS_TOP_LEVEL)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
    add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)
else ()
    # Bundled builds must use the same game headers and SDK as their host.
    set(DUSKLIGHT_DIR "${CMAKE_SOURCE_DIR}")
endif ()

include(FetchContent)
include(cmake/LibJuice.cmake)
if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    block()
        set(BUILD_SHARED_LIBS OFF)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(online_opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG v1.5.2
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(online_opus)
        set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endblock()
endif ()
if (NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()

if (NOT TARGET libzstd_static AND NOT TARGET zstd::libzstd)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
endif ()
if (TARGET libzstd_static)
    set(ONLINE_ZSTD_TARGET libzstd_static)
else ()
    set(ONLINE_ZSTD_TARGET zstd::libzstd)
endif ()

# The native world-label renderer uses a private ImGui font atlas. No host
# ImGui internals are imported.
FetchContent_Declare(
    online_imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_GetProperties(online_imgui)
if (NOT online_imgui_POPULATED)
    FetchContent_Populate(online_imgui)
endif ()

# Chat observes the SDL event already owned and dispatched by Dusklight; it
# does not link or initialize a second SDL runtime. Standalone mod builds need
# only the matching public event/key definitions.
find_path(DUSKLIGHT_ONLINE_SDL3_INCLUDE_DIR SDL3/SDL_events.h
    HINTS
        "${DUSKLIGHT_DIR}/build/windows-msvc/_deps/sdl3_prebuilt-src/include"
        "${DUSKLIGHT_DIR}/build/windows-msvc/_deps/sdl3-src/include"
)
if (NOT DUSKLIGHT_ONLINE_SDL3_INCLUDE_DIR)
    FetchContent_Declare(online_sdl3_headers
        URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.10.tar.gz"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_GetProperties(online_sdl3_headers)
    if (NOT online_sdl3_headers_POPULATED)
        FetchContent_Populate(online_sdl3_headers)
    endif ()
    set(DUSKLIGHT_ONLINE_SDL3_INCLUDE_DIR "${online_sdl3_headers_SOURCE_DIR}/include")
endif ()

add_library(dusklight_online_imgui STATIC
    "${online_imgui_SOURCE_DIR}/imgui.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(dusklight_online_imgui PUBLIC "${online_imgui_SOURCE_DIR}")
target_compile_features(dusklight_online_imgui PUBLIC cxx_std_20)
set_target_properties(dusklight_online_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Remote Link's actor files and font resources are included with this project
# so a clean checkout has every input needed to configure the build. The rest
# of the mod compiles against the SDK selected above.
set(DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dusklight_remote_link"
)

# Fail closed on every imported file so an explicitly supplied checkout cannot
# silently change Remote Link behavior or packaged resources.
function(dusklight_remote_link_require_sha256 relative_path expected_sha256)
    set(source_path "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/${relative_path}")
    if (NOT EXISTS "${source_path}")
        message(FATAL_ERROR "Pinned Remote Link source file is missing: ${relative_path}")
    endif ()
    if (relative_path MATCHES "\\.(cpp|h|hpp)$")
        # Git normalizes checkout line endings. Pin source content, not the
        # platform-specific LF/CRLF representation; keep binary assets exact.
        file(READ "${source_path}" source_text)
        string(REPLACE "\r\n" "\n" source_text "${source_text}")
        string(SHA256 actual_sha256 "${source_text}")
    else ()
        file(SHA256 "${source_path}" actual_sha256)
    endif ()
    string(TOLOWER "${expected_sha256}" expected_sha256_lower)
    if (NOT actual_sha256 STREQUAL expected_sha256_lower)
        message(FATAL_ERROR
            "Bundled Remote Link source file has an unexpected hash: "
            "${relative_path} (got ${actual_sha256})")
    endif ()
endfunction()

dusklight_remote_link_require_sha256(
    "src/d/actor/d_a_remote_link.cpp"
    "4728d71e32c7aa826fa48bfaabbf8dca4932162be5fbc40530ec3451ce8f5c9e")
dusklight_remote_link_require_sha256(
    "include/d/actor/d_a_remote_link.h"
    "4bb8819df3e3c339d12df8fd697649692914a6b520e48f2c632bbde738f27499")
dusklight_remote_link_require_sha256(
    "src/dusk/multiplayer/remote_link_dummy.cpp"
    "8d863ac9c687d4049f54a0f21c85edaf2ed667997ba7700523a1cc7b857c1d67")
dusklight_remote_link_require_sha256(
    "include/dusk/multiplayer/remote_link_dummy.hpp"
    "0B5041A924C629747CDE888F126850B83A30C496AD1DFD633C22B0032E6BE28E")
dusklight_remote_link_require_sha256(
    "res/AlegreyaSC-Bold.ttf"
    "F0EDAF1F289B7EB1B94BA9290146D57E5B413364BCF723CD0196779FAA557AFA")
dusklight_remote_link_require_sha256(
    "res/Inter-Regular.ttf"
    "712FB3AE7568CC3812776EDA49922FF8BCA5521CB02C006DEC80040E9260C262")

set(DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/src")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/d/actor")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}")
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/include/d/actor/d_a_remote_link.h"
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/d/actor/d_a_remote_link.h"
    COPYONLY
)
set(DUSKLIGHT_ONLINE_GENERATED_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/res")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/icon.png"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/banner.png"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/banner.png"
    COPYONLY
)
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/res/AlegreyaSC-Bold.ttf"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/AlegreyaSC-Bold.ttf"
    COPYONLY
)
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/res/Inter-Regular.ttf"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/Inter-Regular.ttf"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/FONT_LICENSES.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/FONT_LICENSES.txt"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/THIRD_PARTY_LICENSES.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/THIRD_PARTY_LICENSES.txt"
    COPYONLY
)
foreach (emoji_license EMOJI_APACHE_LICENSE.txt GEMOJI_LICENSE.txt STB_IMAGE_LICENSE.txt)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/res/${emoji_license}"
        "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/${emoji_license}"
        COPYONLY
    )
endforeach ()
file(GLOB DUSKLIGHT_ONLINE_EMOJI_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/emoji/*.png")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/emoji")
foreach (emoji_asset IN LISTS DUSKLIGHT_ONLINE_EMOJI_ASSETS)
    get_filename_component(emoji_filename "${emoji_asset}" NAME)
    configure_file("${emoji_asset}"
        "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/emoji/${emoji_filename}" COPYONLY)
endforeach ()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/OPUS_LICENSE.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/OPUS_LICENSE.txt"
    COPYONLY
)
# The selected SDK does not expose Z2AudioMgr::seStartNoCull. Preserve the
# actor source apart from this asserted call-site adaptation.
file(READ
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/src/d/actor/d_a_remote_link.cpp"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
)
set(DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL "audioMgr->seStartNoCull(")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL}" DUSKLIGHT_ONLINE_REMOTE_NO_CULL_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_NO_CULL_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link no-cull call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL}"
    "dusklight_online::game::remote_audio_start_no_cull(audioMgr, "
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
# JASGlobalInstance<T> inline accessors bind to the consuming DLL's template
# static on Windows. Route Remote Link audio helpers through compatibility
# functions instead of permitting null mod-local copies. In particular,
# getAudibleSwFull is not a stable mod ABI symbol, so its helper uses the
# default distance-volume curve without importing that private game method.
set(DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL
    "Z2GetSoundInfo()->getAudibleSwFull(i_soundId)")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL}"
    DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link sound-info call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL}"
    "dusklight_online::game::remote_audio_audible_params(i_soundId)"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
set(DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL "Z2GetAudience()")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL}"
    DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link audience call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL}"
    "dusklight_online::game::remote_audio_audience()"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
file(WRITE
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}/d_a_remote_link_impl.inc"
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/dusk/multiplayer")
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/include/dusk/multiplayer/remote_link_dummy.hpp"
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/dusk/multiplayer/remote_link_dummy.hpp"
    COPYONLY
)
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/src/dusk/multiplayer/remote_link_dummy.cpp"
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}/remote_link_dummy_impl.inc"
    COPYONLY
)

set(ONLINE_INVITE_SOURCES
    src/net/invite_code.cpp
)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/LIBJUICE_LICENSE.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/LIBJUICE_LICENSE.txt" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher/LICENCE.md"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/MONOCYPHER_LICENSE.txt" COPYONLY)

set(ONLINE_CONNECTION_SOURCES
    src/net/ice_agent.cpp
    src/net/reliable_udp.cpp
    src/net/datagram_scheduler.cpp
    src/net/secure_datagram.cpp
    src/net/udp_connection.cpp
    vendor/kcp/ikcp.c
    vendor/monocypher/monocypher.c
)
link_libraries(juice-static)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/kcp")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/vendor/monocypher")
if (WIN32)
    link_libraries(bcrypt)
endif ()

set(ONLINE_CORE_SOURCES
    ${ONLINE_CONNECTION_SOURCES}
    ${ONLINE_INVITE_SOURCES}
    src/net/transport.cpp
    src/net/auth_code.cpp
    src/net/cloud_cipher.cpp
    src/net/udp_codec.cpp
)

set(ONLINE_GAME_SOURCES
    src/game/chat_emoji.cpp
    src/game/chat_emoji_atlas.cpp
    src/game/voice_chat.cpp
    src/game/player_color.cpp
    src/game/appearance.cpp
    src/game/audio_bridge.cpp
    src/game/bomb_bridge.cpp
    src/game/collectible_visual_bridge.cpp
    src/game/floor_switch_bridge.cpp
    src/game/coop_features.cpp
    src/game/game_adapter.cpp
    src/game/local_pose.cpp
    src/game/protocol_router.cpp
    src/game/randomizer_check_names.cpp
    src/game/randomizer_item_names.cpp
    src/game/remote_pose.cpp
    src/game/remote_actor_bridge.cpp
    src/game/speedrun_bridge.cpp
    src/game/visual_bridge.cpp
    src/game/remote_link.cpp
    src/game/remote_link_dummy.cpp
)

add_mod(dusklight_online
    FEATURES game
    SOURCES
        src/mod.cpp
        src/online_app.cpp
        src/net/sdk_room_channel.cpp
        ${ONLINE_CORE_SOURCES}
        ${ONLINE_GAME_SOURCES}
    BUNDLE
    MOD_JSON mod.json
    RES_DIR "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}"
)

target_include_directories(dusklight_online PRIVATE
    include
    src
    vendor/stb
    "${DUSKLIGHT_ONLINE_SDL3_INCLUDE_DIR}"
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}"
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}"
    "${DUSKLIGHT_DIR}/src"
)
# Remote Link is supplied by this mod rather than mainline's profile table.
# Keep it on a private s16 ID and substitute the profile only for process IDs
# created by this mod. This avoids colliding with GAMEOVER or a future profile
# assigned a private value outside the host's process-name range.
target_compile_definitions(dusklight_online PRIVATE fpcNm_REMOTE_LINK_e=0x7FFE)
target_link_libraries(dusklight_online PRIVATE
    nlohmann_json::nlohmann_json
    ${ONLINE_ZSTD_TARGET}
    dusklight_online_imgui
    ${CMAKE_DL_LIBS}
)
if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(dusklight_online PRIVATE opus)
    target_compile_definitions(dusklight_online PRIVATE DUSKLIGHT_ONLINE_VOICE=1)
endif ()
if (WIN32)
    target_link_libraries(dusklight_online PRIVATE ws2_32 user32)
    # MSVC otherwise records the absolute build-machine PDB path in mod.dll's
    # PE debug directory. Keep distributed .dusk packages free of usernames
    # and local workspace paths while retaining a useful PDB basename.
    target_link_options(dusklight_online PRIVATE "/PDBALTPATH:dusklight_online.pdb")
endif ()

# The relay is intentionally a standalone/headless operator process. It uses
# the same invite-code source as the mod so endpoint codes cannot drift.
option(DUSKLIGHT_ONLINE_BUILD_RELAY "Build the desktop relay operator tools" ${PROJECT_IS_TOP_LEVEL})
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/multiplayer_relay/version.cmake")
if (DUSKLIGHT_ONLINE_BUILD_RELAY AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(dusklight_online_relay
        tools/multiplayer_relay/relay.cpp
        src/net/auth_code.cpp
        ${ONLINE_CONNECTION_SOURCES}
        ${ONLINE_INVITE_SOURCES}
    )
    target_compile_features(dusklight_online_relay PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_relay PRIVATE include src)
    target_link_libraries(dusklight_online_relay PRIVATE nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    target_compile_definitions(dusklight_online_relay PRIVATE
        DUSKLIGHT_RELAY_VERSION="${DUSKLIGHT_ONLINE_RELAY_VERSION}")

    if (WIN32)
        target_link_libraries(dusklight_online_relay PRIVATE ws2_32 winmm)
        if (MSVC)
            # This combined build shares the mod's Zstandard/CRT configuration.
            # Use tools/multiplayer_relay for the standalone static-CRT
            # distribution. Keep the absolute PDB path out of this binary too.
            set_property(TARGET dusklight_online_relay PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            target_link_options(dusklight_online_relay PRIVATE
                "/PDBALTPATH:dusklight_online_relay.pdb")
        endif ()
        add_executable(dusklight_online_relay_launcher WIN32
            tools/multiplayer_relay/launcher.cpp
            ${ONLINE_INVITE_SOURCES}
        )
        target_compile_features(dusklight_online_relay_launcher PRIVATE cxx_std_20)
        target_include_directories(dusklight_online_relay_launcher PRIVATE include src)
        target_compile_definitions(dusklight_online_relay_launcher PRIVATE
            DUSKLIGHT_RELAY_VERSION="${DUSKLIGHT_ONLINE_RELAY_VERSION}")
        target_link_libraries(dusklight_online_relay_launcher PRIVATE
            nlohmann_json::nlohmann_json
            ws2_32
            shell32
        )
        if (MSVC)
            set_property(TARGET dusklight_online_relay_launcher PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            target_link_options(dusklight_online_relay_launcher PRIVATE
                "/PDBALTPATH:dusklight_online_relay_launcher.pdb")
        endif ()
    endif ()
endif ()

option(DUSKLIGHT_ONLINE_BUILD_TESTS "Build standalone online core tests" ${PROJECT_IS_TOP_LEVEL})
if (DUSKLIGHT_ONLINE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(dusklight_online_mesh_transport_test
        tests/mesh_transport_test.cpp ${ONLINE_CONNECTION_SOURCES} src/net/udp_codec.cpp)
    target_include_directories(dusklight_online_mesh_transport_test PRIVATE include)
    target_compile_features(dusklight_online_mesh_transport_test PRIVATE cxx_std_20)
    target_link_libraries(dusklight_online_mesh_transport_test PRIVATE nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    if (WIN32)
        target_link_libraries(dusklight_online_mesh_transport_test PRIVATE ws2_32)
    endif ()
    add_test(NAME dusklight_online_mesh_transport COMMAND dusklight_online_mesh_transport_test)
    set_tests_properties(dusklight_online_mesh_transport PROPERTIES TIMEOUT 180)
    add_executable(dusklight_online_player_color_test tests/player_color_test.cpp src/game/player_color.cpp)
    target_compile_features(dusklight_online_player_color_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_player_color_test PRIVATE include)
    add_test(NAME dusklight_online_player_color COMMAND dusklight_online_player_color_test)

    add_executable(dusklight_online_trade_item_sync_test tests/trade_item_sync_test.cpp)
    target_compile_features(dusklight_online_trade_item_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_trade_item_sync_test PRIVATE include)
    add_test(NAME dusklight_online_trade_item_sync COMMAND dusklight_online_trade_item_sync_test)

    add_executable(dusklight_online_bottle_sync_test tests/bottle_sync_test.cpp)
    target_compile_features(dusklight_online_bottle_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_bottle_sync_test PRIVATE include)
    add_test(NAME dusklight_online_bottle_sync COMMAND dusklight_online_bottle_sync_test)

    add_executable(dusklight_online_bomb_bag_sync_test tests/bomb_bag_sync_test.cpp)
    target_compile_features(dusklight_online_bomb_bag_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_bomb_bag_sync_test PRIVATE include)
    add_test(NAME dusklight_online_bomb_bag_sync COMMAND dusklight_online_bomb_bag_sync_test)

    add_executable(dusklight_online_floor_switch_sync_test tests/floor_switch_sync_test.cpp)
    target_compile_features(dusklight_online_floor_switch_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_floor_switch_sync_test PRIVATE include)
    add_test(NAME dusklight_online_floor_switch_sync COMMAND dusklight_online_floor_switch_sync_test)

    add_executable(dusklight_online_floor_switch_wire_test tests/floor_switch_wire_test.cpp)
    target_compile_features(dusklight_online_floor_switch_wire_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_floor_switch_wire_test PRIVATE include)
    target_link_libraries(dusklight_online_floor_switch_wire_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_floor_switch_wire COMMAND dusklight_online_floor_switch_wire_test)

    add_executable(dusklight_online_ooccoo_sync_test tests/ooccoo_sync_test.cpp)
    target_compile_features(dusklight_online_ooccoo_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_ooccoo_sync_test PRIVATE include)
    add_test(NAME dusklight_online_ooccoo_sync COMMAND dusklight_online_ooccoo_sync_test)

    add_executable(dusklight_online_ooccoo_wire_test tests/ooccoo_wire_test.cpp)
    target_compile_features(dusklight_online_ooccoo_wire_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_ooccoo_wire_test PRIVATE include)
    target_link_libraries(dusklight_online_ooccoo_wire_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_ooccoo_wire COMMAND dusklight_online_ooccoo_wire_test)

    add_executable(dusklight_online_cave_map_sync_test tests/cave_map_sync_test.cpp)
    target_compile_features(dusklight_online_cave_map_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_cave_map_sync_test PRIVATE include)
    add_test(NAME dusklight_online_cave_map_sync COMMAND dusklight_online_cave_map_sync_test)

    add_executable(dusklight_online_poe_sync_test tests/poe_sync_test.cpp)
    target_compile_features(dusklight_online_poe_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_poe_sync_test PRIVATE include)
    add_test(NAME dusklight_online_poe_sync COMMAND dusklight_online_poe_sync_test)
    # Compile the actual appearance implementation against an isolated fake
    # engine/service to exercise resource ownership without launching Dusk.
    add_executable(dusklight_online_appearance_service_test
        tests/appearance_service/test.cpp src/game/player_color.cpp)
    target_compile_features(dusklight_online_appearance_service_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_appearance_service_test PRIVATE
        tests/appearance_service include src/game)
    add_test(NAME dusklight_online_appearance_service
        COMMAND dusklight_online_appearance_service_test)
    add_executable(dusklight_online_particle_depth_test tests/particle_depth_test.cpp)
    target_compile_features(dusklight_online_particle_depth_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_particle_depth_test PRIVATE include)
    add_test(NAME dusklight_online_particle_depth COMMAND dusklight_online_particle_depth_test)
    add_executable(dusklight_online_invite_code_test
        tests/invite_code_test.cpp
        ${ONLINE_INVITE_SOURCES}
    )
    target_compile_features(dusklight_online_invite_code_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_invite_code_test PRIVATE include src)
    target_link_libraries(dusklight_online_invite_code_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_invite_code COMMAND dusklight_online_invite_code_test)

    add_executable(dusklight_online_auth_code_test
        tests/auth_code_test.cpp src/net/auth_code.cpp)
    target_compile_features(dusklight_online_auth_code_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_auth_code_test PRIVATE include)
    add_test(NAME dusklight_online_auth_code COMMAND dusklight_online_auth_code_test)

    add_executable(dusklight_online_cloud_cipher_test
        tests/cloud_cipher_test.cpp src/net/cloud_cipher.cpp src/net/auth_code.cpp
        vendor/monocypher/monocypher.c)
    target_compile_features(dusklight_online_cloud_cipher_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_cloud_cipher_test PRIVATE include)
    target_link_libraries(dusklight_online_cloud_cipher_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_cloud_cipher COMMAND dusklight_online_cloud_cipher_test)

    add_executable(dusklight_online_secure_datagram_test
        tests/secure_datagram_test.cpp src/net/secure_datagram.cpp
        vendor/monocypher/monocypher.c)
    target_compile_features(dusklight_online_secure_datagram_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_secure_datagram_test PRIVATE include)
    add_test(NAME dusklight_online_secure_datagram
             COMMAND dusklight_online_secure_datagram_test)

    add_executable(dusklight_online_secure_udp_connection_test
        tests/secure_udp_connection_test.cpp ${ONLINE_CONNECTION_SOURCES})
    target_compile_features(dusklight_online_secure_udp_connection_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_secure_udp_connection_test PRIVATE include)
    if (WIN32)
        target_link_libraries(dusklight_online_secure_udp_connection_test PRIVATE ws2_32)
    endif ()
    add_test(NAME dusklight_online_secure_udp_connection
             COMMAND dusklight_online_secure_udp_connection_test)

    add_executable(dusklight_online_transport_test
        tests/transport_test.cpp
        ${ONLINE_CORE_SOURCES}
    )
    target_compile_features(dusklight_online_transport_test PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_online_transport_test PRIVATE DUSKLIGHT_DIRECT_SECURITY_TESTING=1)
    target_include_directories(dusklight_online_transport_test PRIVATE include src)
    target_link_libraries(dusklight_online_transport_test PRIVATE
        nlohmann_json::nlohmann_json
        ${ONLINE_ZSTD_TARGET}
    )
    if (WIN32)
        target_link_libraries(dusklight_online_transport_test PRIVATE ws2_32)
    endif ()
    add_test(NAME dusklight_online_transport COMMAND dusklight_online_transport_test)

    add_executable(dusklight_online_cloud_room_transport_test
        tests/cloud_room_transport_test.cpp
        ${ONLINE_CORE_SOURCES}
    )
    target_compile_features(dusklight_online_cloud_room_transport_test PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_online_cloud_room_transport_test PRIVATE DUSKLIGHT_CLOUD_SECURITY_TESTING=1)
    target_include_directories(dusklight_online_cloud_room_transport_test PRIVATE include src)
    target_link_libraries(dusklight_online_cloud_room_transport_test PRIVATE
        nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    if (WIN32)
        target_link_libraries(dusklight_online_cloud_room_transport_test PRIVATE ws2_32)
    endif ()
    add_test(NAME dusklight_online_cloud_room_transport
             COMMAND dusklight_online_cloud_room_transport_test)

    add_executable(dusklight_online_udp_codec_test
        tests/udp_codec_test.cpp
        src/net/udp_codec.cpp
    )
    target_compile_features(dusklight_online_udp_codec_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_udp_codec_test PRIVATE include src)
    target_link_libraries(dusklight_online_udp_codec_test PRIVATE
        nlohmann_json::nlohmann_json
        ${ONLINE_ZSTD_TARGET}
    )
    add_test(NAME dusklight_online_udp_codec COMMAND dusklight_online_udp_codec_test)

    add_executable(dusklight_online_coop_rules_test tests/coop_rules_test.cpp)
    target_compile_features(dusklight_online_coop_rules_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_coop_rules_test PRIVATE include)
    target_link_libraries(dusklight_online_coop_rules_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_coop_rules COMMAND dusklight_online_coop_rules_test)

    add_executable(dusklight_online_protocol_router_test
        tests/protocol_router_test.cpp
        src/game/protocol_router.cpp
    )
    target_compile_features(dusklight_online_protocol_router_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_protocol_router_test PRIVATE include src)
    target_link_libraries(dusklight_online_protocol_router_test PRIVATE
        nlohmann_json::nlohmann_json
    )
    add_test(NAME dusklight_online_protocol_router COMMAND dusklight_online_protocol_router_test)

    add_executable(dusklight_online_save_recovery_test tests/save_recovery_test.cpp)
    target_compile_features(dusklight_online_save_recovery_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_save_recovery_test PRIVATE include)
    target_link_libraries(dusklight_online_save_recovery_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_save_recovery COMMAND dusklight_online_save_recovery_test)

    add_executable(dusklight_online_pickup_sync_test tests/pickup_sync_test.cpp)
    target_compile_features(dusklight_online_pickup_sync_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_pickup_sync_test PRIVATE include)
    add_test(NAME dusklight_online_pickup_sync COMMAND dusklight_online_pickup_sync_test)

    add_executable(dusklight_online_randomizer_item_names_test
        tests/randomizer_item_names_test.cpp
        src/game/randomizer_item_names.cpp
    )
    target_compile_features(dusklight_online_randomizer_item_names_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_randomizer_item_names_test PRIVATE include)
    add_test(NAME dusklight_online_randomizer_item_names
             COMMAND dusklight_online_randomizer_item_names_test)

    add_executable(dusklight_online_chat_emoji_test
        tests/chat_emoji_test.cpp
        src/game/chat_emoji.cpp
    )
    target_compile_features(dusklight_online_chat_emoji_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_chat_emoji_test PRIVATE include)
    add_test(NAME dusklight_online_chat_emoji
             COMMAND dusklight_online_chat_emoji_test)

    add_executable(dusklight_online_randomizer_check_names_test
        tests/randomizer_check_names_test.cpp
        src/game/randomizer_check_names.cpp
    )
    target_compile_features(dusklight_online_randomizer_check_names_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_randomizer_check_names_test PRIVATE include)
    add_test(NAME dusklight_online_randomizer_check_names
             COMMAND dusklight_online_randomizer_check_names_test)

    add_executable(dusklight_online_remote_pose_test
        tests/remote_pose_test.cpp
        src/game/remote_pose.cpp
    )
    target_compile_features(dusklight_online_remote_pose_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_remote_pose_test PRIVATE include src)
    target_link_libraries(dusklight_online_remote_pose_test PRIVATE
        nlohmann_json::nlohmann_json
    )
    add_test(NAME dusklight_online_remote_pose COMMAND dusklight_online_remote_pose_test)

    add_executable(dusklight_online_audio_lifetime_test tests/audio_lifetime_test.cpp)
    target_compile_features(dusklight_online_audio_lifetime_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_audio_lifetime_test PRIVATE include)
    add_test(NAME dusklight_online_audio_lifetime COMMAND dusklight_online_audio_lifetime_test)

    add_executable(dusklight_online_relay_transport_client_test
        tests/relay_transport_client_test.cpp
        ${ONLINE_CORE_SOURCES}
    )
    target_compile_features(dusklight_online_relay_transport_client_test PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_online_relay_transport_client_test PRIVATE DUSKLIGHT_TRANSPORT_TESTING=1)
    target_include_directories(dusklight_online_relay_transport_client_test PRIVATE include src)
    target_link_libraries(dusklight_online_relay_transport_client_test PRIVATE
        nlohmann_json::nlohmann_json
        ${ONLINE_ZSTD_TARGET}
    )
    if (WIN32)
        target_link_libraries(dusklight_online_relay_transport_client_test PRIVATE ws2_32 winmm)
    endif ()

    # Opt-in measurement target: run with an external relay and controlled
    # network impairment. It is not a timing assertion in the default suite.
    add_executable(dusklight_online_relay_fanout_benchmark EXCLUDE_FROM_ALL
        tests/reliable_udp/relay_fanout_benchmark.cpp ${ONLINE_CORE_SOURCES})
    target_compile_features(dusklight_online_relay_fanout_benchmark PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_online_relay_fanout_benchmark PRIVATE DUSKLIGHT_TRANSPORT_TESTING=1)
    target_include_directories(dusklight_online_relay_fanout_benchmark PRIVATE include src)
    target_link_libraries(dusklight_online_relay_fanout_benchmark PRIVATE nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    if (WIN32)
        target_link_libraries(dusklight_online_relay_fanout_benchmark PRIVATE ws2_32 winmm)
    endif ()

    add_executable(dusklight_online_pose_ack_history_test tests/pose_ack_history_test.cpp)
    target_compile_features(dusklight_online_pose_ack_history_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_pose_ack_history_test PRIVATE include)
    add_test(NAME dusklight_online_pose_ack_history COMMAND dusklight_online_pose_ack_history_test)

    add_executable(dusklight_online_pose_playback_test tests/pose_playback_test.cpp)
    target_compile_features(dusklight_online_pose_playback_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_pose_playback_test PRIVATE include)
    add_test(NAME dusklight_online_pose_playback COMMAND dusklight_online_pose_playback_test)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_library(dusklight_online_udp_test_client SHARED
        tests/udp_test_client.cpp ${ONLINE_CONNECTION_SOURCES})
    target_compile_features(dusklight_online_udp_test_client PRIVATE cxx_std_20)
    target_compile_definitions(dusklight_online_udp_test_client PRIVATE DUSKLIGHT_TRANSPORT_TESTING=1)
    target_include_directories(dusklight_online_udp_test_client PRIVATE include)
    target_link_libraries(dusklight_online_udp_test_client PRIVATE nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    if(WIN32)
        target_link_libraries(dusklight_online_udp_test_client PRIVATE ws2_32)
    endif()
    add_executable(dusklight_online_reliability_test
        tests/reliable_udp/reliability_test.cpp ${ONLINE_CONNECTION_SOURCES})
    target_compile_features(dusklight_online_reliability_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_reliability_test PRIVATE include)
    if(WIN32)
        target_link_libraries(dusklight_online_reliability_test PRIVATE ws2_32)
    endif()
    add_executable(dusklight_online_reliable_json_test tests/reliable_json_test.cpp)
    target_compile_features(dusklight_online_reliable_json_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_reliable_json_test PRIVATE include)
    target_link_libraries(dusklight_online_reliable_json_test PRIVATE nlohmann_json::nlohmann_json ${ONLINE_ZSTD_TARGET})
    add_test(NAME dusklight_online_reliable_json COMMAND dusklight_online_reliable_json_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/reliable_udp/full_progression_upper.json")
    # Keep assertion-based tests effective in Release too, and prevent Windows
    # assertion/abort dialogs from blocking unattended verification.
    get_property(online_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach (test_target IN LISTS online_test_targets)
        if (test_target MATCHES "_test$")
            if (MSVC)
                target_compile_options(${test_target} PRIVATE /UNDEBUG
                    "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime.hpp>")
            else ()
                target_compile_options(${test_target} PRIVATE -UNDEBUG)
            endif ()
        endif ()
    endforeach ()

    add_test(NAME dusklight_online_reliability COMMAND dusklight_online_reliability_test)
    set_tests_properties(dusklight_online_reliability PROPERTIES TIMEOUT 30)
    if (Python3_Interpreter_FOUND AND TARGET dusklight_online_relay)
        add_test(
            NAME dusklight_online_relay_integration
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/multiplayer_relay/test_relay.py"
                --relay $<TARGET_FILE:dusklight_online_relay>
                --udp-library $<TARGET_FILE:dusklight_online_udp_test_client>
        )
        set_tests_properties(dusklight_online_relay_integration PROPERTIES TIMEOUT 120)
        add_test(NAME dusklight_online_relay_version
                 COMMAND $<TARGET_FILE:dusklight_online_relay> --version)
        set_tests_properties(dusklight_online_relay_version PROPERTIES
            PASS_REGULAR_EXPRESSION
                "dusklight_online_relay ${DUSKLIGHT_ONLINE_RELAY_VERSION}")
        add_test(
            NAME dusklight_online_relay_transport
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_relay_transport_test.py"
                $<TARGET_FILE:dusklight_online_relay>
                $<TARGET_FILE:dusklight_online_relay_transport_client_test>
        )
        set_tests_properties(dusklight_online_relay_transport PROPERTIES TIMEOUT 60)
        foreach (case IN ITEMS fallback_local fallback_wan fallback_loss direct_wan direct_asymmetric)
            if (case STREQUAL "fallback_local")
                set(case_args --fallback)
            elseif (case STREQUAL "fallback_wan")
                set(case_args --fallback 140 0.02)
            elseif (case STREQUAL "direct_wan")
                set(case_args --direct-wan 140 0.02)
            elseif (case STREQUAL "direct_asymmetric")
                set(case_args --direct-asymmetric 140 0.02)
            else ()
                set(case_args --fallback 70 0.05)
            endif ()
            add_test(NAME dusklight_online_delivery_${case}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_relay_transport_test.py"
                    $<TARGET_FILE:dusklight_online_relay> $<TARGET_FILE:dusklight_online_relay_transport_client_test> ${case_args})
            set_tests_properties(dusklight_online_delivery_${case} PROPERTIES TIMEOUT 100)
        endforeach ()
    endif ()
endif ()
