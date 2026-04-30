#include <stdio.h>
#include <cuda_runtime.h>

__global__ void saxpy(int n, float a, float *x, float *y) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        y[i] = a * x[i] + y[i];
    }
}

int main() {
    int N = 1382400;
    size_t size = N * sizeof(float);

    float *x, *y;
    float *d_x, *d_y;

    // Allocate pinned (page-locked) host memory
    cudaMallocHost((void**)&x, size);
    cudaMallocHost((void**)&y, size);

    // Allocate device memory
    cudaMalloc((void**)&d_x, size);
    cudaMalloc((void**)&d_y, size);

    // Initialize data
    for (int i = 0; i < N; i++) {
        x[i] = 2.0f;
        y[i] = 2.0f;
    }

    // Create a CUDA stream
    cudaStream_t stream;
    cudaStreamCreate(&stream);

    // Async copy host -> device
    cudaMemcpyAsync(d_x, x, size, cudaMemcpyHostToDevice, stream);
    cudaMemcpyAsync(d_y, y, size, cudaMemcpyHostToDevice, stream);

    // Launch kernel in the same stream
    int blockSize = 256;
    int gridSize = (N + blockSize - 1) / blockSize;
    saxpy<<<gridSize, blockSize, 0, stream>>>(N, 2.0f, d_x, d_y);

    // Async copy device -> host
    cudaMemcpyAsync(y, d_y, size, cudaMemcpyDeviceToHost, stream);

    // Wait for all operations in stream to complete
    cudaStreamSynchronize(stream);

    // Cleanup
    cudaStreamDestroy(stream);
    cudaFree(d_x);
    cudaFree(d_y);
    cudaFreeHost(x);
    cudaFreeHost(y);

    return 0;
}