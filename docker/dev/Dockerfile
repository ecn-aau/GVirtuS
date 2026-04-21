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
    wget \
    && rm -rf /var/lib/apt/lists/*


ENV GVIRTUS_HOME=/opt/GVirtuS
    
COPY ./msquic ${GVIRTUS_HOME}/msquic
RUN cd ${GVIRTUS_HOME}/msquic &&  \
    cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build && \
    cmake --install build

ENV DEBIAN_FRONTEND=noninteractive

COPY ./src/ ${GVIRTUS_HOME}/src/
COPY ./include/ ${GVIRTUS_HOME}/include/
COPY ./etc/ ${GVIRTUS_HOME}/etc/
COPY ./cmake/ ${GVIRTUS_HOME}/cmake/
COPY ./lib/ ${GVIRTUS_HOME}/lib/
COPY ./plugins/ ${GVIRTUS_HOME}/plugins/
COPY ./tests/ ${GVIRTUS_HOME}/tests/
COPY ./CMakeLists.txt ${GVIRTUS_HOME}/CMakeLists.txt

WORKDIR ${GVIRTUS_HOME}
RUN cmake -B build -DCMAKE_INSTALL_PREFIX=${GVIRTUS_HOME} \
    -DCMAKE_SHARED_LINKER_FLAGS="-L/usr/local/cuda-12.6/targets/x86_64-linux/lib/stubs -L/usr/local/cuda/lib64/stubs" && \
    cd build && \
    make -j$(nproc) && make install

ENV LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:/usr/local/lib:${LD_LIBRARY_PATH}
WORKDIR ${GVIRTUS_HOME}
