## This Dockerfile is for building a GVirtuS image with CUDA support.
## It does not include the Gvirtus source code, as it is expected to be mounted at runtime (useful for development).
## The image will be pushed at taslanidis/gvirtus-dependencies:cuda12.6.3-cudnn-ubuntu22.04 using the `make docker-build-push-dev` command.

FROM nvidia/cuda:12.6.3-cudnn-devel-ubuntu22.04
RUN apt update && apt install -y --no-install-recommends \
    build-essential \
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

ARG MSQUIC_VERSION=2.5.6
RUN wget -q https://packages.microsoft.com/config/ubuntu/22.04/packages-microsoft-prod.deb \
    && dpkg -i packages-microsoft-prod.deb \
    && rm packages-microsoft-prod.deb \
    && apt update \
    && apt install -y --no-install-recommends libmsquic=${MSQUIC_VERSION} \
    && rm -rf /var/lib/apt/lists/*

ENV GVIRTUS_HOME=/usr/local/gvirtus
ENV GVIRTUS_LOGLEVEL=0
ENV LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:${LD_LIBRARY_PATH}

COPY ./docker/dev/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]
