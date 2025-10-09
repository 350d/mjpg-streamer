FROM debian:bookworm

# Build argument for target architecture
ARG TARGET_ARCH=arm64

# Set cross-compilation variables based on architecture
RUN case "${TARGET_ARCH}" in \
        arm64) \
            echo "CROSS_TRIPLE=aarch64-linux-gnu" > /etc/cross-compile-env; \
            ;; \
        armhf) \
            echo "CROSS_TRIPLE=arm-linux-gnueabihf" > /etc/cross-compile-env; \
            ;; \
        *) \
            echo "Unsupported architecture: ${TARGET_ARCH}" >&2; exit 1; \
            ;; \
    esac

# Add target architecture
RUN dpkg --add-architecture ${TARGET_ARCH}

# Install base development tools and cross-compilation dependencies
RUN . /etc/cross-compile-env && \
    apt-get update && apt-get install -y \
    build-essential \
    debhelper \
    debhelper-compat \
    devscripts \
    fakeroot \
    pkg-config \
    cmake \
    libjpeg-dev \
    libv4l-dev \
    libqrencode-dev \
    libdbus-1-dev \
    gcc-${CROSS_TRIPLE} \
    g++-${CROSS_TRIPLE} \
    crossbuild-essential-${TARGET_ARCH} \
    libjpeg-dev:${TARGET_ARCH} \
    libv4l-dev:${TARGET_ARCH} \
    libqrencode-dev:${TARGET_ARCH} \
    libdbus-1-dev:${TARGET_ARCH} \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the source code
COPY . /src/

# Copy and install local quirc .deb packages for the target architecture
RUN echo "Installing quirc packages for ${TARGET_ARCH}..." && \
    ls -la /src/libquirc*.deb && \
    # Install packages for the target architecture
    if [ -f /src/libquirc1_1.2+deb12-1_${TARGET_ARCH}.deb ] && [ -f /src/libquirc-dev_1.2+deb12-1_${TARGET_ARCH}.deb ]; then \
        echo "Installing ${TARGET_ARCH} quirc packages..."; \
        dpkg -i /src/libquirc1_1.2+deb12-1_${TARGET_ARCH}.deb /src/libquirc-dev_1.2+deb12-1_${TARGET_ARCH}.deb; \
    else \
        echo "WARNING: ${TARGET_ARCH} quirc packages not found - QR scanner plugin will be disabled for ${TARGET_ARCH} builds"; \
    fi && \
    echo "Quirc packages installation completed" && \
    find /usr -name '*quirc*' 2>/dev/null || echo "No quirc files found"

# Make build script executable
RUN chmod +x /src/build.sh

# Default command
CMD ["./build.sh"]
