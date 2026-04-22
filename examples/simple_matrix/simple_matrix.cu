// Usage: ./simple_matrix_app [communicator] [iterations] [size] [output.csv]
//   communicator  label written to CSV, e.g. "tcp" or "quic"  (default: "unknown")
//   iterations    number of runs                               (default: 100)
//   size          matrix dimension N — computes N×N * N×N      (default: 1000)
//   output.csv    path to the CSV output file                  (default: "profiling_results.csv")
//
// CSV columns: run, communicator, warmup, size, setup_us, h2d_us, sgemm_us, d2h_us, teardown_us, total_us

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
    int         N            = (argc > 3) ? std::atoi(argv[3]) : 100;
    const char *outfile      = (argc > 4) ? argv[4] : "profiling_results.csv";

    if (iterations <= 0 || N <= 0) {
        std::cerr << "Error: iterations and size must be positive integers.\n";
        return 1;
    }

    const long long elems_AB = (long long)N * N;
    const long long elems_C  = (long long)N * N;

    // Heap-allocate host matrices A (N×N), B (N×N), C (N×N)
    float *A = new float[elems_AB];
    float *B = new float[elems_AB];
    float *C = new float[elems_C]();   // zero-initialised

    for (long long i = 0; i < elems_AB; ++i) {
        A[i] = 1.0f;
        B[i] = 1.0f;
    }

    const float alpha = 1.0f;
    const float beta  = 0.0f;

    std::ofstream csv(outfile);
    if (!csv.is_open()) {
        std::cerr << "Error: cannot open \"" << outfile << "\" for writing.\n";
        delete[] A; delete[] B; delete[] C;
        return 1;
    }
    csv << "run,communicator,warmup,size,setup_us,h2d_us,sgemm_us,d2h_us,teardown_us,total_us\n";

    std::cout << "Communicator: " << communicator << "  iterations: " << iterations
              << "  size: " << N << "x" << N << "  output: " << outfile << "\n\n";

    for (int iter = 0; iter < iterations; ++iter) {
        float         *d_A, *d_B, *d_C;
        cublasHandle_t handle;

        // Setup: allocate device memory + create cuBLAS handle
        auto t0 = hrc::now();
        cudaMalloc((void **)&d_A, elems_AB * sizeof(float));
        cudaMalloc((void **)&d_B, elems_AB * sizeof(float));
        cudaMalloc((void **)&d_C, elems_C  * sizeof(float));
        cublasCreate(&handle);
        auto t1 = hrc::now();

        // H2D: copy A and B to device
        cudaMemcpy(d_A, A, elems_AB * sizeof(float), cudaMemcpyHostToDevice);
        cudaMemcpy(d_B, B, elems_AB * sizeof(float), cudaMemcpyHostToDevice);
        auto t2 = hrc::now();

        // SGEMM: C = alpha * A * B + beta * C  (A:NxN, B:NxN, C:NxN)
        cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &alpha, d_A, N, d_B, N, &beta, d_C, N);
        auto t3 = hrc::now();

        // D2H: copy result back
        cudaMemcpy(C, d_C, elems_C * sizeof(float), cudaMemcpyDeviceToHost);
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

        csv << (iter + 1)   << "," << communicator << "," << is_warmup << "," << N << ","
            << setup_us     << "," << h2d_us       << "," << sgemm_us  << ","
            << d2h_us       << "," << teardown_us  << "," << total_us  << "\n";

        std::cout << "[" << (iter + 1) << "/" << iterations << "]"
                  << "  total=" << total_us << " us"
                  << "  (setup=" << setup_us << " h2d=" << h2d_us
                  << " sgemm=" << sgemm_us << " d2h=" << d2h_us
                  << " tear=" << teardown_us << ")\n";
    }

    // Sanity check: C[0] should equal N (all-ones A * all-ones B)
    std::cout << "\nC[0,0]=" << C[0] << " (expected " << N << ")\n"
              << "\nDone. Results written to: " << outfile << "\n";

    csv.close();
    delete[] A;
    delete[] B;
    delete[] C;
    return 0;
}
