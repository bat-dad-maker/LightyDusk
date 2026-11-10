cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/version.cmake")
project(dusklight_online_relay VERSION ${DUSKLIGHT_ONLINE_RELAY_VERSION} LANGUAGES C CXX)
find_package(Threads REQUIRED)

include(FetchContent)

option(DUSKLIGHT_RELAY_BUILD_LAUNCHER "Build the Windows relay launcher" ON)
option(DUSKLIGHT_RELAY_BUILD_TESTS "Build and register relay tests" ON)

find_package(nlohmann_json 3.12 QUIET)
if (NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()

get_filename_component(DUSKLIGHT_ONLINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if (MSVC)
    file(TO_CMAKE_PATH "${DUSKLIGHT_ONLINE_ROOT}" DUSKLIGHT_RELAY_SOURCE_ROOT)
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" DUSKLIGHT_RELAY_BINARY_ROOT)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" DUSKLIGHT_RELAY_USER_ROOT)
    add_compile_options(
        "/experimental:deterministic"
        "/pathmap:${DUSKLIGHT_RELAY_SOURCE_ROOT}=Z:\\relay-src"
        "/pathmap:${DUSKLIGHT_RELAY_BINARY_ROOT}=Z:\\relay-build"
    )
    if (DUSKLIGHT_RELAY_USER_ROOT)
        add_compile_options("/pathmap:${DUSKLIGHT_RELAY_USER_ROOT}=Z:\\redacted")
    endif ()
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build"
        "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build"
    )
endif ()

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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


include("${DUSKLIGHT_ONLINE_ROOT}/cmake/LibJuice.cmake")

function(configure_relay_target target pdb_name)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE
        "${DUSKLIGHT_ONLINE_ROOT}/include"
        "${DUSKLIGHT_ONLINE_ROOT}/src"
        "${DUSKLIGHT_ONLINE_ROOT}/vendor/kcp"
        "${DUSKLIGHT_ONLINE_ROOT}/vendor/monocypher"
    )
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json Threads::Threads juice-static)
    if (WIN32)
        target_link_libraries(${target} PRIVATE bcrypt)
    endif ()
    target_compile_definitions(${target} PRIVATE
        DUSKLIGHT_RELAY_VERSION="${PROJECT_VERSION}")
    if (MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE /W4)
        target_link_options(${target} PRIVATE /Brepro "/PDBALTPATH:${pdb_name}")
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endfunction()

set(DUSKLIGHT_RELAY_COMMON_SOURCES
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/invite_code.cpp"
)

add_executable(dusklight_online_relay
    relay.cpp
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/auth_code.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/ice_agent.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/reliable_udp.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/datagram_scheduler.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/secure_datagram.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/src/net/udp_connection.cpp"
    "${DUSKLIGHT_ONLINE_ROOT}/vendor/kcp/ikcp.c"
    "${DUSKLIGHT_ONLINE_ROOT}/vendor/monocypher/monocypher.c"
    ${DUSKLIGHT_RELAY_COMMON_SOURCES}
)
configure_relay_target(dusklight_online_relay dusklight_online_relay.pdb)
target_link_libraries(dusklight_online_relay PRIVATE ${ONLINE_ZSTD_TARGET})
if (WIN32)
    target_link_libraries(dusklight_online_relay PRIVATE ws2_32 winmm)
endif ()

if (WIN32 AND DUSKLIGHT_RELAY_BUILD_LAUNCHER)
    add_executable(dusklight_online_relay_launcher WIN32
        launcher.cpp
        ${DUSKLIGHT_RELAY_COMMON_SOURCES}
    )
    configure_relay_target(
        dusklight_online_relay_launcher dusklight_online_relay_launcher.pdb)
    target_link_libraries(dusklight_online_relay_launcher PRIVATE ws2_32 shell32)
endif ()

if (DUSKLIGHT_RELAY_BUILD_TESTS)
    enable_testing()

    add_executable(dusklight_relay_invite_code_test
        "${DUSKLIGHT_ONLINE_ROOT}/tests/invite_code_test.cpp"
        ${DUSKLIGHT_RELAY_COMMON_SOURCES}
    )
    configure_relay_target(
        dusklight_relay_invite_code_test dusklight_relay_invite_code_test.pdb)
    add_test(NAME dusklight_relay_invite_code COMMAND dusklight_relay_invite_code_test)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_library(dusklight_online_udp_test_client SHARED
        "${DUSKLIGHT_ONLINE_ROOT}/src/net/ice_agent.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/tests/udp_test_client.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/src/net/reliable_udp.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/src/net/datagram_scheduler.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/src/net/secure_datagram.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/src/net/udp_connection.cpp"
        "${DUSKLIGHT_ONLINE_ROOT}/vendor/kcp/ikcp.c"
        "${DUSKLIGHT_ONLINE_ROOT}/vendor/monocypher/monocypher.c")
    configure_relay_target(dusklight_online_udp_test_client udp_test_client.pdb)
    target_compile_definitions(dusklight_online_udp_test_client PRIVATE DUSKLIGHT_TRANSPORT_TESTING=1)
    target_link_libraries(dusklight_online_udp_test_client PRIVATE ${ONLINE_ZSTD_TARGET})
    if(WIN32)
        target_link_libraries(dusklight_online_udp_test_client PRIVATE ws2_32)
    endif()
    add_test(
        NAME dusklight_relay_integration
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_relay.py"
                --relay $<TARGET_FILE:dusklight_online_relay>
                --udp-library $<TARGET_FILE:dusklight_online_udp_test_client>
    )
    set_tests_properties(dusklight_relay_integration PROPERTIES TIMEOUT 120)
    add_test(NAME dusklight_relay_version
             COMMAND $<TARGET_FILE:dusklight_online_relay> --version)
    set_tests_properties(dusklight_relay_version PROPERTIES
        PASS_REGULAR_EXPRESSION "dusklight_online_relay ${PROJECT_VERSION}")
endif ()

install(TARGETS dusklight_online_relay RUNTIME DESTINATION . COMPONENT Relay)
if (TARGET dusklight_online_relay_launcher)
    install(TARGETS dusklight_online_relay_launcher RUNTIME DESTINATION . COMPONENT Relay)
endif ()
install(FILES
    README.md
    "${DUSKLIGHT_ONLINE_ROOT}/LICENSE.md"
    THIRD_PARTY_NOTICES.md
    DESTINATION . COMPONENT Relay
)
install(FILES PROTOCOL.md DEPLOYMENT.md SYNCING.md DESTINATION docs COMPONENT Relay)
install(FILES "${DUSKLIGHT_ONLINE_ROOT}/vendor/kcp/LICENSE" DESTINATION licenses RENAME KCP.txt COMPONENT Relay)
install(FILES "${DUSKLIGHT_ONLINE_ROOT}/res/LIBJUICE_LICENSE.txt" DESTINATION licenses COMPONENT Relay)
install(FILES "${DUSKLIGHT_ONLINE_ROOT}/vendor/monocypher/LICENCE.md"
    DESTINATION licenses RENAME MONOCYPHER_LICENSE.txt COMPONENT Relay)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "dusklight-online-relay")
set(CPACK_PACKAGE_VENDOR "Dusklight Online")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "dusklight-online-relay-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_RESOURCE_FILE_LICENSE "${DUSKLIGHT_ONLINE_ROOT}/LICENSE.md")
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Relay;/")
include(CPack)
