#!/bin/bash
set -e

# MJPG-streamer build script
# This script builds the Debian package for the specified architecture

echo "Starting build..."

# Get architecture from command line argument or environment variable
ARCH=${1:-${ARCH:-arm64}}

echo "Building mjpg-streamer for architecture: ${ARCH}"

# Validate architecture
if [[ ! "${ARCH}" =~ ^(arm64|armhf)$ ]]; then
    echo "Unsupported architecture: ${ARCH}"
    echo "Supported architectures: arm64, armhf"
    exit 1
fi

echo "Building for supported architecture: ${ARCH}"

# Create a writable workspace in /tmp
BUILD_DIR="/tmp/mjpg-streamer-build"
echo "Creating writable build directory: ${BUILD_DIR}"
rm -rf ${BUILD_DIR}
cp -r /src ${BUILD_DIR}
cd ${BUILD_DIR}

# Build the package using Debian's cross-compilation support
echo "Building Debian package for ${ARCH}..."

# Source cross-compilation environment if it exists
if [ -f /etc/cross-compile-env ]; then
    echo "Sourcing cross-compilation environment..."
    . /etc/cross-compile-env
    echo "Cross-compilation environment: CROSS_TRIPLE=${CROSS_TRIPLE}"
fi

# Set build options for dpkg-buildpackage
export DEB_BUILD_OPTIONS="parallel=$(nproc)"

# Note: CC, CXX, and PKG_CONFIG_PATH are automatically handled by debhelper v13+
# when using dpkg-buildpackage with -Pcross flag
echo "Using debhelper's automatic cross-compilation support for ${ARCH}"

dpkg-buildpackage -a${ARCH} -Pcross -uc -us -b

# Copy built packages to output directory
echo "Copying build artifacts to /output..."
mkdir -p /output
cp ../*.deb /output/ 2>/dev/null || true
cp ../*.changes /output/ 2>/dev/null || true
cp ../*.buildinfo /output/ 2>/dev/null || true

echo "Build completed successfully!"
echo "Available packages in /output/:"
ls -la /output/
