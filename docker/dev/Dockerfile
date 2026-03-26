## This Dockerfile is for building a GVirtuS image with CUDA support.
## It does not include the Gvirtus source code, as it is expected to be mounted at runtime (useful for development).
## The image will be pushed at taslanidis/gvirtus-dependencies:cuda12.6.3-cudnn-ubuntu22.04 using the `make docker-build-push-dev` command.

FROM nvidia/cuda:12.6.3-cudnn-devel-ubuntu22.04
RUN apt update && apt install -y --no-install-recommends \
    build-essential \
    git \
    libssl-dev \
    ca-certificates \
    libxmu-dev \
    libxi-dev \
    libgl-dev \
    libosmesa-dev \
    liblog4cplus-dev \
    librdmacm-dev \
    libibverbs-dev \
    libgtest-dev \
    liblz4-dev \
    cmake \
    && rm -rf /var/lib/apt/lists/*

ENV GVIRTUS_HOME=/usr/local/gvirtus
ENV GVIRTUS_LOGLEVEL=0
ENV LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:usr/local/lib:${LD_LIBRARY_PATH}

# Clone MsQuic
RUN git clone --recurse-submodules https://github.com/microsoft/msquic.git && \
    cd msquic && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build && \
    cmake --install build
    

# ---- Ensure runtime linker can find libmsquic ----
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/msquic.conf && ldconfig

# Optional: sanity check (helps debugging builds)
RUN ldconfig -p | grep msquic || true
# # Build
# RUN 
# RUN cmake --build build

# # Optional: install
# RUN cmake --install build

COPY ./docker/dev/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
