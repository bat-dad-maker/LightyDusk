# Build from source with the caller's toolchain/runtime, never SM64 archives.
# v1.6.2 annotated tag resolves to this immutable source commit.
if (NOT TARGET juice-static)
    include(FetchContent)
    set(NO_TESTS ON)
    FetchContent_Declare(online_juice
        GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
        GIT_TAG 85efaa9b5e1cb3d4d534fc85d69cc9f7b76a66d7)
    FetchContent_MakeAvailable(online_juice)
    # Also cover source overrides outside the repository/build roots.
    file(TO_CMAKE_PATH "${online_juice_SOURCE_DIR}" online_juice_mapped_source)
    foreach(target juice juice-static)
        if (MSVC)
            target_compile_options(${target} PRIVATE /experimental:deterministic
                "/pathmap:${online_juice_mapped_source}=Z:\\third-party\\libjuice")
        elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE
                "-ffile-prefix-map=${online_juice_mapped_source}=third-party/libjuice")
        endif ()
    endforeach ()
endif ()
