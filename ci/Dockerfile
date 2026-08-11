FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    zip \
    unzip \
    python3 \
    python3-venv \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    gcc-arm-none-eabi \
    binutils-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    curl \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/venv
RUN /opt/venv/bin/pip install --no-cache-dir conan==2.6.0
RUN chmod -R a+rX /opt/venv

ENV PATH="/opt/venv/bin:$PATH"

WORKDIR /workspace

ENTRYPOINT ["/bin/bash"]
