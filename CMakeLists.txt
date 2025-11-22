cmake_minimum_required(VERSION 3.15)

project(bwfs
    VERSION 1.0.0
    DESCRIPTION "Black and White File System"
    LANGUAGES C
)

# Generate compile_commands.json for IDE support (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")

# ============================================================================
# libfuse Configuration
# ============================================================================

# Option to build libfuse from source (experimental)
option(BUILD_LIBFUSE "Build libfuse from source in external directory" OFF)

# Use local libfuse headers from external directory
set(LIBFUSE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/libfuse/include")
set(LIBFUSE_HEADERS_DIR "${LIBFUSE_INCLUDE_DIR}")


message(STATUS "Dir for libfuse: ${LIBFUSE_HEADERS_DIR}")


# Verify headers exist
if(NOT EXISTS "${LIBFUSE_HEADERS_DIR}/fuse.h")
    message(FATAL_ERROR "libfuse headers not found at ${LIBFUSE_HEADERS_DIR}")
endif()

message(STATUS "Using local libfuse headers from: ${LIBFUSE_INCLUDE_DIR}")

# Try to find libfuse3 library for linking
# First try pkg-config
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET fuse3)
endif()

# If pkg-config didn't find it, try find_library with various names
if(NOT FUSE3_FOUND)
    # Try different library names for different platforms
    if(APPLE)
        # macOS: try macFUSE, osxfuse, and fuse3
        find_library(FUSE3_LIBRARY
            NAMES osxfuse fuse3 macfuse
            PATHS
                /usr/local/lib
                /opt/homebrew/lib
                /opt/lib
        )
    else()
        # Linux/Unix: try fuse3
        find_library(FUSE3_LIBRARY
            NAMES fuse3
            PATHS
                /usr/lib
                /usr/local/lib
                /opt/lib
        )
    endif()
    
    if(FUSE3_LIBRARY)
        set(FUSE3_FOUND TRUE)
        set(FUSE3_LIBRARIES ${FUSE3_LIBRARY})
        message(STATUS "Found libfuse library: ${FUSE3_LIBRARY}")
    else()
        message(WARNING "libfuse3 library not found. Linking may fail.")
        if(APPLE)
            message(WARNING "")
            message(WARNING "For macOS, you need to install macFUSE:")
            message(WARNING "  Option 1: Download from https://osxfuse.github.io/")
            message(WARNING "  Option 2: brew install macfuse")
            message(WARNING "")
            message(WARNING "After installation, the library should be in /usr/local/lib")
        else()
            message(WARNING "")
            message(WARNING "For Linux, install the development package:")
            message(WARNING "  Debian/Ubuntu: sudo apt-get install libfuse3-dev")
            message(WARNING "  RHEL/CentOS:   sudo yum install fuse3-devel")
            message(WARNING "  Arch:          sudo pacman -S fuse3")
            message(WARNING "")
        endif()
        set(FUSE3_LIBRARIES "")
    endif()
else()
    message(STATUS "Found libfuse3 via pkg-config")
endif()

# Include directories
include_directories(${LIBFUSE_INCLUDE_DIR})
if(FUSE3_FOUND AND FUSE3_INCLUDE_DIRS)
    include_directories(${FUSE3_INCLUDE_DIRS})
endif()

# ============================================================================
# Source Files
# ============================================================================

set(BWFS_SOURCES
    src/bwfs.c
)

set(MKFS_BWFS_SOURCES
    src/mkfs_bwfs.c
)

set(MOUNT_BWFS_SOURCES
    src/mount_bwfs.c
)

set(SERVER_SOURCES
    src/network/server.c
    src/network/protocol.c
)

set(CLIENT_SOURCES
    src/network/client.c
    src/network/protocol.c
)

set(TEST_MESSAGE_SEND_SOURCES
    src/network/test_message_send.c
    src/network/client.c
    src/network/protocol.c
)

# ============================================================================
# Executables
# ============================================================================

# mkfs.bwfs - filesystem formatter (doesn't need libfuse)
add_executable(mkfs.bwfs ${MKFS_BWFS_SOURCES})
target_include_directories(mkfs.bwfs PRIVATE ${LIBFUSE_INCLUDE_DIR})

# mount.bwfs - mount wrapper (doesn't need libfuse)
add_executable(mount.bwfs ${MOUNT_BWFS_SOURCES})
target_include_directories(mount.bwfs PRIVATE ${LIBFUSE_INCLUDE_DIR})

# Network server (Berkeley Sockets)
add_executable(bwfs-server ${SERVER_SOURCES})
target_include_directories(bwfs-server PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

# Test message send executable (client test program)
add_executable(test_message_send ${TEST_MESSAGE_SEND_SOURCES})
target_include_directories(test_message_send PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

# Main FUSE daemon (requires libfuse library)
if(FUSE3_FOUND)
    # Configure META_FILENAME path at compile time
    # Default to project root directory, but allow override via CMake cache
    if(NOT DEFINED BWFS_META_FILENAME)
        set(BWFS_META_FILENAME "${CMAKE_SOURCE_DIR}/bwfs_metadata.bin" CACHE STRING "Path to bwfs_metadata.bin file")
    endif()
    
    add_executable(bwfs ${BWFS_SOURCES})
    target_include_directories(bwfs PRIVATE ${LIBFUSE_INCLUDE_DIR})
    
    # Define META_FILENAME as a compile-time constant
    target_compile_definitions(bwfs PRIVATE META_FILENAME="${BWFS_META_FILENAME}")
    
    # Define FUSE version macros (required by libfuse headers)
    # FUSE_USE_VERSION 31 means version 3.1
    target_compile_definitions(bwfs PRIVATE 
        FUSE_MAJOR_VERSION=3
        FUSE_MINOR_VERSION=1
        FUSE_HOTFIX_VERSION=0
    )
    
    if(FUSE3_LIBRARIES)
        target_link_libraries(bwfs PRIVATE ${FUSE3_LIBRARIES})
    endif()
    if(FUSE3_LDFLAGS_OTHER)
        target_link_options(bwfs PRIVATE ${FUSE3_LDFLAGS_OTHER})
    endif()
    
    message(STATUS "bwfs executable will be built")
    message(STATUS "  META_FILENAME: ${BWFS_META_FILENAME}")
else()
    # Library not found - provide helpful error but don't fail immediately
    # Allow mkfs.bwfs and mount.bwfs to be built
    if(APPLE)
        message(WARNING
            "\n"
            "═══════════════════════════════════════════════════════════════\n"
            "  WARNING: libfuse library not found!\n"
            "═══════════════════════════════════════════════════════════════\n"
            "\n"
            "The 'bwfs' executable will NOT be built.\n"
            "However, 'mkfs.bwfs' and 'mount.bwfs' can still be built.\n"
            "\n"
            "For macOS, you need to install macFUSE to build 'bwfs':\n"
            "\n"
            "  Option 1 (Recommended):\n"
            "    brew install macfuse\n"
            "\n"
            "  Option 2:\n"
            "    Download from https://osxfuse.github.io/\n"
            "    Install the .pkg file\n"
            "\n"
            "After installation, run cmake again.\n"
            "\n"
            "Note: You may need to restart your terminal after installation.\n"
            "═══════════════════════════════════════════════════════════════\n"
        )
    else()
        message(WARNING
            "\n"
            "═══════════════════════════════════════════════════════════════\n"
            "  WARNING: libfuse3 library not found!\n"
            "═══════════════════════════════════════════════════════════════\n"
            "\n"
            "The 'bwfs' executable will NOT be built.\n"
            "However, 'mkfs.bwfs' and 'mount.bwfs' can still be built.\n"
            "\n"
            "Please install the libfuse3 development package:\n"
            "\n"
            "  Debian/Ubuntu:  sudo apt-get install libfuse3-dev\n"
            "  RHEL/CentOS:     sudo yum install fuse3-devel\n"
            "  Fedora:          sudo dnf install fuse3-devel\n"
            "  Arch Linux:      sudo pacman -S fuse3\n"
            "  openSUSE:        sudo zypper install fuse3-devel\n"
            "\n"
            "After installation, run cmake again.\n"
            "═══════════════════════════════════════════════════════════════\n"
        )
    endif()
    message(STATUS "bwfs executable will NOT be built (libfuse not found)")
endif()

# ============================================================================
# Installation (optional)
# ============================================================================

# Only install targets that were created
set(INSTALL_TARGETS mkfs.bwfs mount.bwfs bwfs-server test_message_send)
if(FUSE3_FOUND)
    list(APPEND INSTALL_TARGETS bwfs)
endif()

install(TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION bin
)

# ============================================================================
# Developer Convenience Targets
# ============================================================================

# Create mount point directory
add_custom_target(mnt
    COMMAND ${CMAKE_COMMAND} -E make_directory mnt
    COMMAND ${CMAKE_COMMAND} -E chmod 777 mnt
    COMMENT "Creating mount point directory"
)

# Create storage directory
add_custom_target(storage
    COMMAND ${CMAKE_COMMAND} -E make_directory bwfs_storage
    COMMAND ${CMAKE_COMMAND} -E chmod 777 bwfs_storage
    COMMENT "Creating storage directory"
)

# Clean target (CMake provides this by default, but we can extend it)
add_custom_target(clean-all
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove -f bwfs_metadata.bin
    COMMAND ${CMAKE_COMMAND} -E remove_directory bwfs_storage
    COMMAND ${CMAKE_COMMAND} -E remove_directory mnt
    COMMENT "Cleaning all build artifacts and generated files"
)

# ============================================================================
# IDE Support
# ============================================================================

# Create symlink to compile_commands.json in source root for IDE support
# Use mkfs.bwfs as the target since it's always built
if(UNIX AND CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_command(TARGET mkfs.bwfs POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_SOURCE_DIR}/compile_commands.json
        COMMENT "Creating compile_commands.json symlink for IDE support"
        BYPRODUCTS ${CMAKE_SOURCE_DIR}/compile_commands.json
    )
endif()

# ============================================================================
# Build Information
# ============================================================================

message(STATUS "")
message(STATUS "=== BWFS Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "libfuse headers: ${LIBFUSE_INCLUDE_DIR}")
message(STATUS "libfuse library: ${FUSE3_LIBRARIES}")

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    message(STATUS "compile_commands.json: Enabled (for IDE support)")
endif()

message(STATUS "")
message(STATUS "To build: cmake -B build && cmake --build build")
message(STATUS "To run: ./build/bwfs mnt")
message(STATUS "")

