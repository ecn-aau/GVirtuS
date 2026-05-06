#include <stdio.h>
#include <cmath>
#include <cuda_runtime.h>
#include <chrono>

__global__ void saxpy(int n, float a, float *x, float *y) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        y[i] = a * x[i] + y[i];
    }
}

int main() {
    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
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

    const int numStreams = 4;
    cudaStream_t streams[numStreams];
    for (int i = 0; i < numStreams; ++i) {
        cudaStreamCreate(&streams[i]);
    }

    int blockSize = 256;
    int chunkSize = (N + numStreams - 1) / numStreams;

    for (int i = 0; i < numStreams; ++i) {
        int offset = i * chunkSize;
        int currentN = (offset + chunkSize <= N) ? chunkSize : (N - offset);
        if (currentN <= 0) break;

        size_t currentSize = currentN * sizeof(float);
        int gridSize = (currentN + blockSize - 1) / blockSize;

        cudaMemcpyAsync(d_x + offset, x + offset, currentSize, cudaMemcpyHostToDevice, streams[i]);
        cudaMemcpyAsync(d_y + offset, y + offset, currentSize, cudaMemcpyHostToDevice, streams[i]);
        saxpy<<<gridSize, blockSize, 0, streams[i]>>>(currentN, 2.0f, d_x + offset, d_y + offset);
        cudaMemcpyAsync(y + offset, d_y + offset, currentSize, cudaMemcpyDeviceToHost, streams[i]);
    }

    for (int i = 0; i < numStreams; ++i) {
        cudaStreamSynchronize(streams[i]);
        cudaStreamDestroy(streams[i]);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    // Verify result
    bool valid = true;
    for (int i = 0; i < N; ++i) {
        float expected = 2.0f * x[i] + 2.0f;
        if (fabsf(y[i] - expected) > 1e-5f) {
            printf("Validation failed at index %d: y=%f expected=%f\n", i, y[i], expected);
            valid = false;
            break;
        }
    }

    if (valid) {
        printf("Result verification passed. %ld ms\n", duration.count());
    }

    cudaFree(d_x);
    cudaFree(d_y);
    cudaFreeHost(x);
    cudaFreeHost(y);

    return valid ? 0 : 1;
}