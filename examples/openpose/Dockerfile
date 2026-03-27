# ===========================================================================
# Stage 1: Build OpenPose, cached independently of GVirtuS.
# ===========================================================================
FROM nvidia/cuda:12.6.3-cudnn-devel-ubuntu22.04 AS openpose-builder

ENV DEBIAN_FRONTEND=noninteractive
ENV OPENPOSE_HOME=/opt/openpose

# Prevent tzdata prompt and install dependencies
RUN ln -fs /usr/share/zoneinfo/Etc/UTC /etc/localtime && \
    echo "Etc/UTC" > /etc/timezone && \
    apt-get update && apt-get install -y tzdata && \
    dpkg-reconfigure -f noninteractive tzdata && \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    wget \
    curl \
    unzip \
    pkg-config \
    libprotobuf-dev \
    protobuf-compiler \
    libgoogle-glog-dev \
    libgflags-dev \
    libatlas-base-dev \
    libboost-all-dev \
    libhdf5-dev \
    libleveldb-dev \
    libsnappy-dev \
    libopencv-dev \
    libopenblas-dev \
    liblmdb-dev \
    python3-dev \
    python3-pip \
    python3-setuptools \
    cmake \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# # Install Python packages
RUN pip3 install opencv-python

# Clone and build OpenPose
WORKDIR /opt
RUN git clone https://github.com/CMU-Perceptual-Computing-Lab/openpose.git

# Copy custom Cuda.cmake
COPY cmake/Cuda.cmake ${OPENPOSE_HOME}/cmake/Cuda.cmake

# Copy patched Caffe (after OpenPose clone)
COPY caffe/ ${OPENPOSE_HOME}/3rdparty/caffe/

# Optional: Copy models directory if needed
COPY models/ ${OPENPOSE_HOME}/models/

WORKDIR ${OPENPOSE_HOME}/build
RUN cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_CAFFE=ON \
    -DBUILD_EXAMPLES=ON \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=OFF \
    -DCUDA_USE_STATIC_CUDA_RUNTIME=OFF \
    && make -j"$(nproc)"

# ===========================================================================
# Final stage: layer OpenPose on top of frontend_base.
# ===========================================================================
FROM gvirtus:cuda12.6 AS final

ENV OPENPOSE_HOME=/opt/openpose
ENV LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:${GVIRTUS_HOME}/lib/frontend:${LD_LIBRARY_PATH}

# Install OpenPose (and Caffe) runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libprotobuf-dev \
    protobuf-compiler \
    libgoogle-glog-dev \
    libgflags-dev \
    libatlas-base-dev \
    libboost-all-dev \
    libhdf5-dev \
    libleveldb-dev \
    libsnappy-dev \
    libopencv-dev \
    libopenblas-dev \
    liblmdb-dev \
    python3-pip \
    python-is-python3 \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install opencv-python

# Copy pre-built OpenPose from stage 1 — no recompilation needed.
COPY --from=openpose-builder /opt/openpose /opt/openpose

WORKDIR /opt/openpose

