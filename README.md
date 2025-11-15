# soa-black-and-white-fs

Black and White File System - A FUSE-based filesystem implementation.

## Project Setup

### Cloning the Repository

If you're cloning this repository for the first time, you need to initialize and update git submodules:

```bash
# Clone with submodules
git clone --recurse-submodules <repository-url>

# Or if you've already cloned without submodules
git submodule update --init --recursive
```

This project uses libfuse3 from the `external/libfuse/` directory for headers, which is included as a git submodule.

### Header Structure

The project is configured to use local libfuse headers:

- Headers are located in `external/libfuse/include/`
- A `fuse3/` subdirectory contains symlinks to match the expected `#include <fuse3/fuse.h>` structure
- A minimal `libfuse_config.h` is provided for compilation

## Building with CMake

This project uses CMake to build the binaries.

### Prerequisites

- CMake 3.15 or higher
- C compiler (GCC or Clang)
- **For `bwfs` executable only**: libfuse3 development libraries:
  - **Linux**: `sudo apt-get install libfuse3-dev` (Debian/Ubuntu) or `sudo yum install fuse3-devel` (RHEL/CentOS)
  - **macOS**: Install [macFUSE](https://osxfuse.github.io/) or use Homebrew: `brew install macfuse`

**Note**: `mkfs.bwfs` and `mount.bwfs` can be built without libfuse. Only `bwfs` requires the library.

**To build `bwfs`**: See [INSTALL.md](INSTALL.md) for detailed installation instructions for libfuse on your platform.

### Build Instructions

#### Standard Build (Recommended)

```bash
# Configure and build
cmake -B build
cmake --build build

# The executables will be in the build/ directory
# Note: bwfs will only be built if libfuse is installed
./build/mkfs.bwfs
./build/mount.bwfs
./build/bwfs  # Only available if libfuse is installed
```

**Important**: If you see a warning about libfuse not being found, `mkfs.bwfs` and `mount.bwfs` will still build successfully. Only `bwfs` requires the libfuse library. Install libfuse to build the complete project.

#### Debug Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

#### Release Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

#### Using CMake GUI

```bash
cmake-gui .
# Configure and Generate, then build with your IDE or make
```

### Creating Directories

CMake provides convenience targets for creating required directories:

```bash
# Create mount point
cmake --build build --target mnt

# Create storage directory
cmake --build build --target storage
```

### Cleaning

```bash
# Clean build artifacts
cmake --build build --target clean

# Clean everything (including build directory)
cmake --build build --target clean-all
```

### IDE Support

CMake automatically generates `compile_commands.json` for IDE support:

- **VS Code**: Install the C/C++ extension - it will automatically detect `compile_commands.json`
- **CLion**: Open the project directory - CLion will detect CMakeLists.txt
- **Vim/Neovim**: Use clangd or coc-clangd with `compile_commands.json`
- **Emacs**: Use eglot or lsp-mode with clangd

The `compile_commands.json` file is created in the `build/` directory and symlinked to the project root after the first build.

## Makefile Wrapper

The project includes a `Makefile` that wraps CMake for convenience. You can use familiar `make` commands:

```bash
make          # Build all executables
make clean    # Clean build artifacts
make mnt      # Create mount point directory
make storage  # Create storage directory
```

**Note**: The Makefile is just a convenience wrapper - all build logic is handled by CMake. For more control, use CMake directly.

## Project Structure

```
.
├── CMakeLists.txt          # CMake build configuration
├── Makefile                # Legacy Makefile (optional)
├── src/
│   ├── bwfs.c             # Main FUSE daemon
│   ├── mkfs_bwfs.c        # Filesystem formatter
│   └── mount_bwfs.c       # Mount wrapper
├── external/
│   └── libfuse/           # Local libfuse headers
└── build/                  # Build directory (generated)
```

## Usage

1. **Format the filesystem:**

   ```bash
   ./build/mkfs.bwfs -c config.ini
   ```

2. **Mount the filesystem:**

   ```bash
   ./build/mount.bwfs -c config.ini mnt/
   ```

3. **Use the filesystem:**

   ```bash
   ls mnt/
   touch mnt/test.txt
   # ... use as normal filesystem
   ```

4. **Unmount:**
   ```bash
   fusermount -u mnt/
   # or on macOS
   umount mnt/
   ```
