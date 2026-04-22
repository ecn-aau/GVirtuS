// Usage: ./simple_matrix_app [communicator] [iterations] [output.csv]
//   communicator  label written to CSV, e.g. "tcp" or "quic"  (default: "unknown")
//   iterations    number of runs                               (default: 100)
//   output.csv    path to the CSV output file                  (default: "profiling_results.csv")
//
// CSV columns: run, communicator, warmup, setup_us, h2d_us, sgemm_us, d2h_us, teardown_us, total_us

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cublas_v2.h>
#include <cuda_runtime.h>

using hrc = std::chrono::high_resolution_clock;

static long elapsed_us(hrc::time_point a, hrc::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

int main(int argc, char *argv[]) {
    const char *communicator = (argc > 1) ? argv[1] : "unknown";
    int         iterations   = (argc > 2) ? std::atoi(argv[2]) : 100;
    const char *outfile      = (argc > 3) ? argv[3] : "profiling_results.csv";

    // Matrix A (2x3), B (3x2), result C (2x2)
    const float A[6] = {1, 2, 3, 4, 5, 6};
    const float B[6] = {7, 8, 9, 10, 11, 12};
    float       C[4] = {0};
    const float alpha = 1.0f;
    const float beta  = 0.0f;

    std::ofstream csv(outfile);
    if (!csv.is_open()) {
        std::cerr << "Error: cannot open \"" << outfile << "\" for writing.\n";
        return 1;
    }
    csv << "run,communicator,warmup,setup_us,h2d_us,sgemm_us,d2h_us,teardown_us,total_us\n";

    std::cout << "Communicator: " << communicator << "  iterations: " << iterations
              << "  output: " << outfile << "\n\n";

    for (int iter = 0; iter < iterations; ++iter) {
        float         *d_A, *d_B, *d_C;
        cublasHandle_t handle;

        // Setup: allocate device memory + create cuBLAS handle
        auto t0 = hrc::now();
        cudaMalloc((void **)&d_A, 6 * sizeof(float));
        cudaMalloc((void **)&d_B, 6 * sizeof(float));
        cudaMalloc((void **)&d_C, 4 * sizeof(float));
        cublasCreate(&handle);
        auto t1 = hrc::now();

        // H2D: copy A and B to device
        cudaMemcpy(d_A, A, 6 * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(d_B, B, 6 * sizeof(float), cudaMemcpyHostToDevice);
        auto t2 = hrc::now();

        // SGEMM: C = alpha * A * B + beta * C  (A:2x3, B:3x2, C:2x2)
        cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, 2, 2, 3, &alpha, d_A, 2, d_B, 3, &beta, d_C, 2);
        auto t3 = hrc::now();

        // D2H: copy result back
        cudaMemcpy(C, d_C, 4 * sizeof(float), cudaMemcpyDeviceToHost);
        auto t4 = hrc::now();

        // Teardown: destroy handle + free device memory
        cublasDestroy(handle);
        cudaFree(d_A);
        cudaFree(d_B);
        cudaFree(d_C);
        auto t5 = hrc::now();

        long setup_us    = elapsed_us(t0, t1);
        long h2d_us      = elapsed_us(t1, t2);
        long sgemm_us    = elapsed_us(t2, t3);
        long d2h_us      = elapsed_us(t3, t4);
        long teardown_us = elapsed_us(t4, t5);
        long total_us    = elapsed_us(t0, t5);
        int  is_warmup   = (iter == 0) ? 1 : 0;

        csv << (iter + 1)   << "," << communicator << "," << is_warmup << ","
            << setup_us     << "," << h2d_us       << "," << sgemm_us  << ","
            << d2h_us       << "," << teardown_us  << "," << total_us  << "\n";

        std::cout << "[" << (iter + 1) << "/" << iterations << "]"
                  << "  total=" << total_us << " us"
                  << "  (setup=" << setup_us << " h2d=" << h2d_us
                  << " sgemm=" << sgemm_us << " d2h=" << d2h_us
                  << " tear=" << teardown_us << ")\n";
    }

    // Print final result for sanity check
    std::cout << "\nMatrix C: ";
    for (int i = 0; i < 4; i++) std::cout << C[i] << " ";
    std::cout << "\n\nDone. Results written to: " << outfile << "\n";

    csv.close();
    return 0;
}
