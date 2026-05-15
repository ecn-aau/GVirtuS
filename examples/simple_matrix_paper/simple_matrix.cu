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
    int N = 1382400;
    float *x, *y, *d_x, *d_y;
    x = (float*)malloc(N * sizeof(float));
    y = (float*)malloc(N * sizeof(float));
    cudaMalloc(&d_x, N * sizeof(float));
    cudaMalloc(&d_y, N * sizeof(float));

    for (int i = 0; i < N; i++) {
        x[i] = 2.0f;
    }

    const int warmupRuns = 1;
    const int numRuns = 100;

    FILE *csv = fopen("simple_matrix_results.csv", "w");
    fprintf(csv, "run,duration_us,passed\n");

    bool allValid = true;
    for (int run = -warmupRuns; run < numRuns; run++) {
        for (int i = 0; i < N; i++) y[i] = 2.0f;

        auto start = std::chrono::high_resolution_clock::now();
        cudaMemcpy(d_x, x, N * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(d_y, y, N * sizeof(float), cudaMemcpyHostToDevice);
        saxpy<<<(N + 255) / 256, 256>>>(N, 2.0f, d_x, d_y);
        cudaMemcpy(y, d_y, N * sizeof(float), cudaMemcpyDeviceToHost);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        bool runValid = true;
        for (int i = 0; i < N; ++i) {
            float expected = 2.0f * x[i] + 2.0f;
            if (fabsf(y[i] - expected) > 1e-5f) {
                runValid = false;
                break;
            }
        }
        if (!runValid) allValid = false;

        if (run >= 0) {
            fprintf(csv, "%d,%ld,%s\n", run + 1, duration.count(), runValid ? "true" : "false");
        }
    }

    fclose(csv);

    if (allValid) {
        printf("Result verification passed. Results written to simple_matrix_results.csv\n");
    } else {
        printf("Some runs failed verification. Results written to simple_matrix_results.csv\n");
    }

    cudaFree(d_x);
    cudaFree(d_y);
    free(x);
    free(y);
    return allValid ? 0 : 1;
}