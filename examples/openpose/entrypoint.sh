#!/bin/bash
set -e

export OPENPOSE_ROOT=/opt/openpose
export GVIRTUS_HOME=/opt/GVirtuS
export LD_LIBRARY_PATH=$OPENPOSE_ROOT/build/src/openpose:$GVIRTUS_HOME/lib:$GVIRTUS_HOME/lib/frontend:$LD_LIBRARY_PATH

# Recompile GVirtuS to ensure the latest changes are included and evaluate error codes
cd ${GVIRTUS_HOME}/build && make -j$(nproc) && make install
if [ $? -ne 0 ]; then
    echo "Error: Failed to compile GVirtuS. Please check the logs for details."
    exit 1
fi

echo "🛠️ Compiling OpenPose test..."
cd /opt/openpose/examples/gvirtus

nvcc 00_test.cpp -o 00_test -g \
  -I$OPENPOSE_ROOT/include \
  -I$OPENPOSE_ROOT/3rdparty/caffe/include \
  -L$OPENPOSE_ROOT/build/src/openpose \
  -L$OPENPOSE_ROOT/build/caffe/lib \
  -lopenpose -lcaffe -lgflags \
  $(pkg-config --cflags --libs opencv4)

echo "🚀 Running OpenPose test..."
cd $OPENPOSE_ROOT
./examples/gvirtus/00_test
