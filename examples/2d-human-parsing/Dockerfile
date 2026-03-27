FROM nvidia/cuda:12.6.3-cudnn-devel-ubuntu22.04 AS human-parsing-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
RUN pip install --no-cache-dir \
    torch torchvision --index-url https://download.pytorch.org/whl/cu126 \
    && pip install --no-cache-dir \
    networkx \
    scipy \
    gdown

#Clone 2D-Human-Parsing and download pretrained model
WORKDIR /opt
RUN git clone https://github.com/fyviezhao/2D-Human-Parsing && \
    cd 2D-Human-Parsing/pretrained/ && \
    python3 -m gdown "https://drive.google.com/uc?id=15C5wsdiczHPiR_kIMNtGPoDXwnd5aNJu"


# Final stage: layer 2D-Human-Parsing on top of frontend_base.
FROM gvirtus:cuda12.6 AS final
ENV LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:${GVIRTUS_HOME}/lib/frontend:${LD_LIBRARY_PATH}

RUN apt-get update && apt-get install -y --no-install-recommends \
    python-is-python3 \
    && rm -rf /var/lib/apt/lists/*

# Copy pre-built 2D-Human-Parsing repo and all pip-installed packages from stage 1.
COPY --from=human-parsing-builder /opt/2D-Human-Parsing /opt/2D-Human-Parsing
COPY --from=human-parsing-builder /usr/local/lib/python3.10/dist-packages /usr/local/lib/python3.10/dist-packages

WORKDIR /opt/2D-Human-Parsing


