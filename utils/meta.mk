# meta.mk - Import configuration from .env file
# This file reads variables from .env and makes them available to Make

# Get the directory where this file is located
UTILS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT_DIR := $(abspath $(UTILS_DIR)/..)

ENV_FILE := $(ROOT_DIR)/.env

# Default values (used if .env doesn't exist or variable is not set)
MAX_FILES ?= 1024
MAX_NAME_LEN ?= 255
META_FILENAME ?= bwfs_metadata.bin
BWFS_MAGIC ?= 0x42574653u

$(if $(wildcard $(ENV_FILE)),$(info Loading .env file from: $(ENV_FILE)),$(info .env file not found, using defaults))

$(info )

-include $(ENV_FILE)

# Export all variables for use in Makefile and subprocesses
export MAX_FILES
export MAX_NAME_LEN
export META_FILENAME
export BWFS_MAGIC

ifndef SILENT
    $(info === BWFS Build Configuration ===)
    $(info MAX_FILES: $(MAX_FILES))
    $(info MAX_NAME_LEN: $(MAX_NAME_LEN))
    $(info META_FILENAME: $(META_FILENAME))
    $(info BWFS_MAGIC: $(BWFS_MAGIC))
    $(info Note: Runtime config (storage_path, max_block_bytes, mount_point) in config.ini)
    $(info ==================================)
endif

$(info )
