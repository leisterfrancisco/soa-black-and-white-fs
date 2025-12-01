# Dockerfile for BWFS (Black and White File System)
FROM ubuntu:latest

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libfuse3-dev \
    git \
    supervisor \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code and configuration
COPY . .

# Initialize git submodules (for external/libfuse headers)
RUN git submodule update --init --recursive

# Remove any existing build directory to avoid CMake cache conflicts
RUN rm -rf build

# Build the project
RUN cmake -B build && \
    cmake --build build

# Copy build artifacts to root directory
RUN cp build/bwfs* ./ && \
    cp build/*bwfs ./

# Note: config.ini, bwfs_storage/, and bwfs_metadata.bin are already copied via COPY . .
# They may not exist on first build but will be created by mkfs.bwfs if needed

# Create mount point directory and supervisor log directory
RUN mkdir -p mnt /var/log/supervisor

# Copy supervisord configuration
COPY supervisord.conf /etc/supervisor/conf.d/supervisord.conf

# Expose port for bwfs-server
EXPOSE 8081

# Run supervisord
CMD ["/usr/bin/supervisord", "-c", "/etc/supervisor/conf.d/supervisord.conf", "-n"]
