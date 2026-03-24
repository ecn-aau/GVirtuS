#include <stdio.h>
#include <sys/wait.h> // for wait()
#include <unistd.h>
#include <thread>
#include <vector>
#include <sys/syscall.h>

__global__
void saxpy(int n, float a, float *x, float *y)
{
  int i = blockIdx.x*blockDim.x + threadIdx.x;
  if (i < n) y[i] = a*x[i] + y[i];
}

void saxpy_loop (void) {
  pid_t tid = syscall(SYS_gettid);
  int N = 1<<20;
  float *x, *y, *d_x, *d_y;
  x = (float*)malloc(N*sizeof(float));
  y = (float*)malloc(N*sizeof(float));

  printf("[tid %u] cudaMalloc1\n", tid);
  cudaMalloc(&d_x, N*sizeof(float));
  printf("[tid %u] cudaMalloc2\n", tid);
  cudaMalloc(&d_y, N*sizeof(float));

  for (int i = 0; i < N; i++) {
    x[i] = 1.0f;
    y[i] = 2.0f;
  }

  printf("[tid %u] cudaMemcpy1\n", tid);
  cudaMemcpy(d_x, x, N*sizeof(float), cudaMemcpyHostToDevice);
  printf("[tid %u] cudaMemcpy2\n", tid);
  cudaMemcpy(d_y, y, N*sizeof(float), cudaMemcpyHostToDevice);
  printf("[tid %u] saxpy\n", tid);
  // Perform SAXPY on 1M elements
  saxpy<<<(N+255)/256, 256>>>(N, 2.0f, d_x, d_y);
  
  printf("[tid %u] cudaMemcpy3\n", tid);
  cudaMemcpy(y, d_y, N*sizeof(float), cudaMemcpyDeviceToHost);

  float maxError = 0.0f;
  for (int i = 0; i < N; i++) {
    maxError = max(maxError, abs(y[i]-4.0f));
    //printf("%f\n", y[i]);
  }
  printf("Max error: %f\n", maxError);
  printf("[tid %u] cudaFree1\n", tid);
  cudaFree(d_x);
  printf("[tid %u] cudaFree2\n", tid);
  cudaFree(d_y);
  free(x);
  free(y);
}

int main(void)
{
  pid_t child_pid, wpid;
  int status = 0;
  //1. variant: run saxpy multiple times - head of line blocking may occur
  /*for (int i=0;i<20;i++)
  {
    saxpy_loop();
  }*/
  //2. variant: run saxpy in individual process - each will transit slow start
    const int numThreads = 1;  // Number of threads to create
    std::vector<std::thread> threads;  // Vector to store the threads

    //saxpy_loop();  
    
    
    for (int i = 0; i < numThreads; ++i) {
        threads.push_back(std::thread(saxpy_loop));  // Create and start thread
        sleep(0.1);
    }

     for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();  // Wait for the thread to finish
        }
    }

    //if ((child_pid = fork()) == 0) {
    //    saxpy_loop();
    //    exit(0);
    //} else {
    //  sleep(1);
    //}
  
 //3. variant: run saxpy in individual threads - TCP: same as 2. QUIC: each thread a stream
  
  
}