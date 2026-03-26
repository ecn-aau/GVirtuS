#! /bin/bash
export GVIRTUS_LOGLEVEL=60000
echo ${GVIRTUS_HOME}
mkdir gvirtus/build && cd gvirtus/build && cmake .. && make -j$(nproc) && make install
ls ${GVIRTUS_HOME}/bin
ls /usr/local
time (${GVIRTUS_HOME}/bin/gvirtus-backend ${GVIRTUS_HOME}/etc/properties.json)

#tail -f /dev/null # for debugging