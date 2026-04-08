# Written By: Theodoros Aslanidis <theodoros.aslanidis@ucdconnect.ie>,
#             School of Computer Science, University College Dublin

project(gvirtus-plugin-cuda)

function(configure_cudadr_plugin)
    find_package(CUDA REQUIRED)

    find_path(CUDADR_INCLUDE_DIRECTORY
        cuda.h
        PATHS ${CUDA_INCLUDE_DIRS})

    if(NOT CUDADR_INCLUDE_DIRECTORY)
        message(WARNING "cuda.h not found, skipping plugins/cudadr")
        return()
    endif()

    message(STATUS "cudaDr include directory found at: ${CUDADR_INCLUDE_DIRECTORY}")

    include_directories(${CUDADR_INCLUDE_DIRECTORY} ${CUDA_INCLUDE_DIRS})

    # Also search CUDA stubs so the plugin builds inside Docker (no real driver present).
    find_library(CUDADR_LIBRARY
        cuda
        PATHS
            ${CUDA_LIBRARIES_PATH}
            /usr/local/cuda/lib64/stubs
            /usr/local/cuda-12.6/targets/x86_64-linux/lib/stubs
            /usr/local/cuda/targets/x86_64-linux/lib/stubs)

    if(NOT CUDADR_LIBRARY)
        message(WARNING "cudaDr library not found, skipping plugins/cudadr")
        return()
    endif()

    message(STATUS "cudaDr library found at ${CUDADR_LIBRARY}, adding plugins/cudadr")

    set(CUDADR_BACKEND_VERSION "${CUDADR_LIBRARY}")
    while(IS_SYMLINK "${CUDADR_BACKEND_VERSION}")
        message(STATUS "${CUDADR_BACKEND_VERSION}")
        execute_process(COMMAND readlink "${CUDADR_BACKEND_VERSION}"
            OUTPUT_VARIABLE CUDADR_BACKEND_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endwhile()
    message(STATUS "Resolved CUDADR version: ${CUDADR_BACKEND_VERSION}")
    string(REGEX REPLACE ".*\\.so\\.([0-9]+(\\.[0-9]+)*)" "\\1" CUDADR_BACKEND_VERSION "${CUDADR_BACKEND_VERSION}")


    gvirtus_add_backend(cuda ${CUDADR_BACKEND_VERSION}
        backend/CudaDrHandler_context.cpp
        backend/CudaDrHandler_device.cpp
        backend/CudaDrHandler_driver_entry_point.cpp
        backend/CudaDrHandler_error.cpp
        backend/CudaDrHandler_event.cpp
        backend/CudaDrHandler_execution.cpp
        backend/CudaDrHandler_initialization.cpp
        backend/CudaDrHandler_memory.cpp
        backend/CudaDrHandler_module.cpp
        backend/CudaDrHandler_occupancy.cpp
        backend/CudaDrHandler_stream.cpp
        backend/CudaDrHandler_streammemory.cpp
        backend/CudaDrHandler_tensormap.cpp
        backend/CudaDrHandler_texture.cpp
        backend/CudaDrHandler_unified.cpp
        backend/CudaDrHandler_version.cpp
        backend/CudaDrHandler_virtmemory.cpp
        backend/CudaDrHandler.cpp
    )

    target_link_libraries(${PROJECT_NAME} ${CUDADR_LIBRARY})

    # libcuda.so always uses soversion 1 (libcuda.so.1) regardless of driver version.
    # This matches the soname PyTorch and other consumers look for via dlopen.
    gvirtus_add_frontend(cuda 1
        frontend/CudaDr_context.cpp
        frontend/CudaDr_device.cpp
        frontend/CudaDr_driver_entry_point.cpp
        frontend/CudaDr_error.cpp
        frontend/CudaDr_event.cpp
        frontend/CudaDr_execution.cpp
        frontend/CudaDr_initialization.cpp
        frontend/CudaDr_memory.cpp
        frontend/CudaDr_module.cpp
        frontend/CudaDr_occupancy.cpp
        frontend/CudaDr_stream.cpp
        frontend/CudaDr_streammemory.cpp
        frontend/CudaDr_tensormap.cpp
        frontend/CudaDr_texture.cpp
        frontend/CudaDr_unified.cpp
        frontend/CudaDr_version.cpp
        frontend/CudaDr_virtmemory.cpp
        frontend/CudaDrFrontend.cpp
    )

endfunction()

# Call the function to configure cudadr plugin
configure_cudadr_plugin()