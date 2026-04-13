/**
 * @mainpage gVirtuS - A GPGPU transparent virtualization component
 *
 * @section Introduction
 * gVirtuS tries to fill the gap between in-house hosted computing clusters,
 * equipped with GPGPUs devices, and pay-for-use high performance virtual
 * clusters deployed  via public or private computing clouds. gVirtuS allows an
 * instanced virtual machine to access GPGPUs in a transparent way, with an
 * overhead  slightly greater than a real machine/GPGPU setup. gVirtuS is
 * hypervisor independent, and, even though it currently virtualizes nVIDIA CUDA
 * based GPUs, it is not limited to a specific brand technology. The performance
 * of the components of gVirtuS is assessed through a suite of tests in
 * different deployment scenarios, such as providing GPGPU power to cloud
 * computing based HPC clusters and sharing remotely hosted GPGPUs among HPC
 * nodes.
 *
 * Written By: Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>,
 *             Department of Applied Science
 */

#include "gvirtus/common/Property.h"

#include <iostream>

using gvirtus::common::Property;

Property &Property::endpoints(const int endpoints) {
    this->_endpoints = endpoints;
    return *this;
}

Property &Property::plugins(const std::vector<std::string> &plugins) {
    _plugins.emplace_back(plugins);
    return *this;
}

Property &Property::secure(bool secure) {
    _secure = secure;
    return *this;
}
