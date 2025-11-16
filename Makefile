# Makefile - Wrapper for CMake build system
# This Makefile provides compatibility for users familiar with 'make'
# All build logic is handled by CMakeLists.txt

# Include configuration from .env file via meta.mk
include utils/meta.mk

.PHONY: all clean mnt storage help

# Default target
all:
	@echo "Using CMake build system..."
	@if [ ! -d build ]; then \
		echo "Configuring CMake..."; \
		cmake -B build -DBWFS_META_FILENAME="$$(pwd)/$(META_FILENAME)"; \
	fi
	@cmake --build build

# Build specific targets
bwfs mkfs.bwfs mount.bwfs: all
	@echo "Built: $@"

# Create directories
# Note: These use hardcoded values matching config.ini defaults
# Runtime mount_point and storage_path are configured in config.ini
mnt:
	@cmake --build build --target mnt 2>/dev/null || \
	(mkdir -p mnt && chmod 777 mnt)

storage:
	@cmake --build build --target storage 2>/dev/null || \
	(mkdir -p bwfs_storage && chmod 777 bwfs_storage)

# Clean targets
clean:
	@cmake --build build --target clean 2>/dev/null || true
	@rm -f bwfs mkfs.bwfs mount.bwfs *.o

clean-all:
	@cmake --build build --target clean-all 2>/dev/null || \
	(rm -rf build bwfs_storage mnt $(META_FILENAME))

# Help
help:
	@echo "BWFS Build System (CMake wrapper)"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build all executables (default)"
	@echo "  clean        - Clean build artifacts"
	@echo "  clean-all    - Clean everything including build directory"
	@echo "  mnt          - Create mount point directory"
	@echo "  storage      - Create storage directory"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Configuration:"
	@echo "  Variables are loaded from .env file via utils/meta.mk"
	@echo "  Edit .env to customize: MAX_FILES, META_FILENAME, etc."
	@echo ""
	@echo "Note: This Makefile is a wrapper for CMake."
	@echo "For more control, use CMake directly:"
	@echo "  cmake -B build"
	@echo "  cmake --build build"
