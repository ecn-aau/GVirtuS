# GVirtuS

The GPU Virtualization Service (GVirtuS) presented in this work tries to fill the gap between in-house hosted computing clusters, equipped with GPGPUs devices, and pay-for-use high performance virtual clusters deployed via public or private computing clouds. gVirtuS allows an instanced virtual machine to access GPGPUs in a transparent and hypervisor independent way, with an overhead slightly greater than a real machine/GPGPU setup. The performance of the components of gVirtuS is assessed through a suite of tests in different deployment scenarios, such as providing GPGPU power to cloud computing based HPC clusters and sharing remotely hosted GPGPUs among HPC nodes.

**Read the official GVirtuS paper [here](https://link.springer.com/chapter/10.1007/978-3-642-15277-1_37).**

# 📄 Published Papers

You can view the full list of all GVirtuS published papers in [CITATIONS](CITATIONS.md).

# How To install GVirtuS Framework and Plugins

## 🧰 Prerequisites

**Tested OS**: Ubuntu 22.04 LTS

Before proceeding, ensure the following dependencies are installed on your system (consider using the [installation script](docs/install_nvidia_cuda_cudnn.sh)):

* `gcc` compiler and toolchain: _Tested with **v11.4.0** (latest verified working version)_

* [CUDA Drivers](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/index.html): _Tested with **v560.35.03** (latest verified working version)_

* [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads): _Tested with CUDA Toolkit **v12.6.3** (latest verified working version)_

* [cuDNN](https://developer.nvidia.com/cudnn-downloads): _Tested with cuDNN **v9.5.1** (latest verified working version)_

* [Docker](https://docs.docker.com/engine/install/): _Ensure Docker Engine is properly installed and running. Latest verified working version **v26.1.3**_

> [!NOTE]
> **CUDA Drivers**, **CUDA Toolkit**, and **cuDNN** only need to be installed on the **host machine** running the **GVirtuS backend**.
> Machines acting as **frontends** do **not** require these installations.

# 📊 GVirtuS Implementation Status

🗂️ Looking for function coverage? Check out the [**STATUS**](./STATUS.md) file for a detailed breakdown of which CUDA functions are:
- 🛠️ Implemented
- 🧪 Tested
- ⚙️ Working

This file tracks progress across major CUDA libraries (e.g., cuBLAS, cuDNN, cuRAND) and helps monitor GVirtuS coverage.


# 🔬 Testing GVirtuS

To test GVirtuS, follow the steps below. This setup runs the GVirtuS backend inside a Docker container with all dependencies pre-installed, and mounts your local source and test files into the container for easy development and debugging.

**1. Start the GVirtuS Backend**

Use the script below to start the GVirtuS backend. It builds GVirtuS from source inside a Docker container and launches the backend process:

```
make docker-build-gvirtus-dependencies
make run-gvirtus-backend-dev
```

> **Note**:
>
> * Before running the application, ensure that the `properties.json` configuration file on the backend & frontend devices contains the correct **IP address**, **port**, and **endpoint suite** of the backend.

### Case 1: Distributed Setup (Different Devices)

If the **GVirtuS backend** is running on a GPU server (or edge device) and the **frontend** is on a different non-GPU device:

* Update the **frontend configuration file** with the backend’s IP and port.

* E.i for OpenPose modify {ROOT_FOLDER}/GVirtuS/examples/openpose/properties.json and {ROOT_FOLDER}/GVirtuS/etc/properties.json
Example configuration: 

  ```json
  {
      "suite": "tcp/ip",
      "protocol": "tcp",
      "server_address": "130.225.243.38",
      "port": "8888"
  }
  ```

---

### Case 2: Local Setup (Same Device)

If both the **GVirtuS backend** and **frontend** are running on the **same server or edge device**:

* No need to find the external IP address.

* Instead, set the server address to **127.0.0.1**:

  ```json
  {
      "suite": "tcp/ip",
      "protocol": "tcp",
      "server_address": "127.0.0.1",
      "port": "8888"
  }
  ```

---

**2. Run the examples**

Build the GVirtuS base to use for the examples

```
docker-build-gvirtus
```

### Simple matrix example
```
make run-simple-matrix-test
```

### 2D Human parsing example
```
make docker-build-2d-human-parsing
make run-2d-human-parsing-test
```

### OpenPose example
```
make docker-build-openpose
make run-openpose-test
```

**3. Run the Tests**

Once the backend is running, you can run the tests using the following script. This script creates a new process inside the same container that acts as the frontend and runs all test files located in the tests/ directory:

```
make run-gvirtus-tests
```

**4. Adding Tests**

To add new tests, simply place your test code in any existing .cu file inside the tests directory. You can also create new .cu files if you wish; just make sure to include them as source files in [tests/CMakeLists.txt](tests/CMakeLists.txt#L32).

**4. Updating and Restarting**

After making local changes to the source or tests:

Stop the currently running GVirtuS backend:

```
make stop-gvirtus
```

Ensure your changes are saved.

Restart the backend and re-run the tests using the scripts above.
---

# ⚠️ Disclaimers

> [!IMPORTANT]
> GVirtuS is currently not production-ready.
> It is **not thread-safe** and has known **memory leaks**. Use it with caution in experimental or non-critical environments.




