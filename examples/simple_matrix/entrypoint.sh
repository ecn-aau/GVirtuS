#!/bin/bash
set -e  # Exit immediately if a command fails

# --- Set environment variables ---
export GVIRTUS_HOME=/opt/GVirtuS
export GVIRTUS_LOGLEVEL=60000
echo ${GVIRTUS_HOME}
ls
mkdir gvirtus/build && cd gvirtus/build && cmake .. && make -j$(nproc) && make install


export EXTRA_NVCCFLAGS='--cudart=shared'
export GVIRTUS_LOGLEVEL=10000
export LD_LIBRARY_PATH=${GVIRTUS_HOME}/lib:${GVIRTUS_HOME}/lib/frontend:${LD_LIBRARY_PATH}

# --- Recompile GVirtuS to ensure the latest changes are included and evaluate error codes---
cd ${GVIRTUS_HOME}/build && make -j$(nproc) && make install
if [ $? -ne 0 ]; then
    echo "Error: Failed to compile GVirtuS. Please check the logs for details."
    exit 1
fi

# --- Navigate to the examples folder ---
cd "${GVIRTUS_HOME}/examples/simple_matrix" || { echo "Failed to enter ${GVIRTUS_HOME}/examples"; exit 1; }

# --- Compile the CUDA program ---
nvcc simple_matrix.cu -o simple_matrix_app \
    -L${GVIRTUS_HOME}/lib/frontend \
    -L${GVIRTUS_HOME}/lib/ \
    -lcuda -lcudart -lcublas 

# --- Run the compiled program ---
./simple_matrix_app
