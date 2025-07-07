#!/bin/bash
set -e

# MJPG-streamer cross-compilation orchestrator script
# Usage: ./build_for_ultimaker.sh [architecture]
# Default architecture is arm64

ARCH=${1:-arm64}
BUILD_DIR="build-${ARCH}"
DOCKER_IMAGE="mjpg-streamer-crossbuild:${ARCH}"

echo "Starting cross-compilation for mjpg-streamer (${ARCH})"

# Validate architecture
if [[ ! "${ARCH}" =~ ^(arm64|armhf)$ ]]; then
    echo "Unsupported architecture: ${ARCH}"
    echo "Supported architectures: arm64, armhf"
    exit 1
fi

echo "Building for supported architecture: ${ARCH}"

# Create output directory
echo "Creating output directory: ${BUILD_DIR}"
mkdir -p ${BUILD_DIR}

# Build Docker image
echo "Building Docker image for cross-compilation..."
docker build -t ${DOCKER_IMAGE} --build-arg TARGET_ARCH=${ARCH} .

# Run cross-compilation in Docker
echo "Running cross-compilation in Docker container..."
docker run --rm \
    -v $(pwd):/src:ro \
    -v $(pwd)/${BUILD_DIR}:/output \
    -w /src \
    -e "ARCH=${ARCH}" \
    ${DOCKER_IMAGE} \
    ./build.sh

echo ""
echo "Cross-compilation completed successfully!"
echo "Built packages are available in: ${BUILD_DIR}/"
echo ""
echo "Package contents:"
ls -la ${BUILD_DIR}/
