# Installing libfuse for BWFS

The `bwfs` executable requires the libfuse library to be installed on your system.

## Git Submodules

This project uses git submodules to include the libfuse headers. Before building, ensure submodules are initialized:

```bash
# If cloning for the first time
git clone --recurse-submodules <repository-url>

# Or if you've already cloned the repository
git submodule update --init --recursive
```

This will populate the `external/libfuse/` directory with the necessary headers.

## macOS Installation

### Option 1: Using Homebrew (Recommended)

```bash
brew install macfuse
```

After installation, you may need to:

1. Restart your terminal
2. Reconfigure CMake: `cmake -B build`
3. Rebuild: `cmake --build build`

### Option 2: Manual Installation

1. Download macFUSE from: https://osxfuse.github.io/
2. Install the `.pkg` file
3. Restart your terminal
4. Reconfigure and rebuild:
   ```bash
   cmake -B build
   cmake --build build
   ```

### Verify Installation

After installing macFUSE, verify it's detected:

```bash
# Check if library exists
ls /usr/local/lib/libosxfuse.dylib

# Or check with pkg-config (if available)
pkg-config --exists osxfuse && echo "Found!"

# Reconfigure CMake to see if it's detected
cmake -B build
```

## Linux Installation

### Debian/Ubuntu

```bash
sudo apt-get update
sudo apt-get install libfuse3-dev
```

### RHEL/CentOS/Fedora

```bash
# RHEL/CentOS
sudo yum install fuse3-devel

# Fedora
sudo dnf install fuse3-devel
```

### Arch Linux

```bash
sudo pacman -S fuse3
```

### openSUSE

```bash
sudo zypper install fuse3-devel
```

## After Installation

Once libfuse is installed:

1. **Reconfigure CMake:**

   ```bash
   cmake -B build
   ```

2. **Rebuild:**

   ```bash
   cmake --build build
   ```

3. **Verify `bwfs` was built:**
   ```bash
   ls -la build/bwfs
   ```

## Troubleshooting

### macOS: Library not found after installation

If CMake still can't find the library after installing macFUSE:

1. **Check library location:**

   ```bash
   find /usr/local /opt/homebrew -name "*fuse*" -o -name "*osxfuse*" 2>/dev/null
   ```

2. **Set library path manually (if needed):**

   ```bash
   export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
   ```

3. **Restart terminal** - macOS may require a fresh terminal session

### Linux: pkg-config not finding fuse3

If pkg-config can't find fuse3:

```bash
# Check if pkg-config file exists
find /usr -name "fuse3.pc" 2>/dev/null

# If found, add to PKG_CONFIG_PATH
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:$PKG_CONFIG_PATH
```

## Building Without libfuse

If you only need `mkfs.bwfs` and `mount.bwfs`, you can build without libfuse:

```bash
cmake -B build
cmake --build build
# Only mkfs.bwfs and mount.bwfs will be built
```

The `bwfs` executable requires libfuse and will be skipped if the library is not found.
