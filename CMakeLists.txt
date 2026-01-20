cmake_minimum_required(VERSION 3.14)
project(vlc_whisper_subs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# 1. Fetch whisper.cpp
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    whisper.cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        master
)
# We only want the library, not the examples
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(WHISPER_SHARED OFF CACHE BOOL "" FORCE)
set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(whisper.cpp)

# -----------------------------------------------------------------------------
# 2. Find VLC SDK
# -----------------------------------------------------------------------------
if (NOT VLC_INCLUDE_DIRS)
    find_package(PkgConfig QUIET)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(VLC libvlc-plugin)
    endif()

    if (NOT VLC_FOUND)
        # Fallback default
        set(VLC_INCLUDE_DIRS "C:/Program Files/VideoLAN/VLC/sdk/include" CACHE PATH "Path to VLC SDK headers")
    endif()
endif()

message(STATUS "VLC Include Dirs: ${VLC_INCLUDE_DIRS}")

# -----------------------------------------------------------------------------
# 3. Define the Plugin
# -----------------------------------------------------------------------------
add_library(whisper_subs SHARED
    modules/whisper_subs/whisper_subs.cpp
)

# Use target-specific includes
target_include_directories(whisper_subs PRIVATE 
    ${VLC_INCLUDE_DIRS}
    ${VLC_INCLUDE_DIRS}/vlc/plugins
)

# Link against internal whisper library
target_link_libraries(whisper_subs PRIVATE whisper)

if (WIN32)
    if (NOT VLC_LIB_DIR)
        set(VLC_LIB_DIR "C:/Program Files/VideoLAN/VLC/sdk/lib" CACHE PATH "Path to VLC SDK libs")
    endif()
    message(STATUS "VLC Lib Dir: ${VLC_LIB_DIR}")

    target_link_directories(whisper_subs PRIVATE ${VLC_LIB_DIR})

    # Usually needed to resolve mdate/var_Inherit/subpicture/etc.
    target_link_libraries(whisper_subs PRIVATE libvlccore libvlc)
endif()

# VLC Plugins are shared libraries but often need specific prefixes/suffixes
set_target_properties(whisper_subs PROPERTIES
    PREFIX ""
    SUFFIX ".dll" # or .so on Linux
)

# Definitions for VLC Plugin
target_compile_definitions(whisper_subs PRIVATE
    __PLUGIN__
    _FILE_OFFSET_BITS=64
    _REENTRANT
    _THREAD_SAFE
)

if (WIN32)
    target_compile_definitions(whisper_subs PRIVATE _WIN32_WINNT=0x0600)
endif()
