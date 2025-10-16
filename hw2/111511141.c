#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

void ini_matrices(unsigned int *A, int dim) {
    for (int i = 0; i < dim; i++) {
        for (int j = 0; j < dim; j++) {
            A[i * dim + j] = i * dim + j;
        }
    }
}

unsigned int getMatrixChecksum(unsigned int *M, int dim) {
    unsigned int checksum = 0;
    for (int i = 0; i < dim * dim; i++) {
        checksum += M[i];
    }
    return checksum;
}

int main() {
    // Let user input the matrix dimension
    int dim;
    printf("Input the matrix dimension: ");
    scanf("%d", &dim);

    // Matrix A and B will be allocated in private memory of the parent process
    unsigned int *matrix_AB =
        (unsigned int *)malloc(dim * dim * sizeof(unsigned int));
    if (matrix_AB == NULL) {
        perror("Matrix A malloc failed");
        exit(1);
    }

    ini_matrices(matrix_AB, dim);

    // Calculate the size needed for shared memory (for matrices C)
    size_t shm_size = dim * dim * sizeof(unsigned int);

    // Create shared memory segment for matrix C
    int shmid = shmget(IPC_PRIVATE, shm_size, IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget failed");
        exit(1);
    }

    // Attach the shared memory segment to this process's address space
    unsigned int *matrix_C = (unsigned int *)shmat(shmid, NULL, 0);
    if (matrix_C == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }

    // 16 cases, degree of process parallelism increases from 1 to 16
    for (int i = 1; i <= 16; i++) {
        // reset matrix C
        for (int j = 0; j < dim * dim; j++) {
            matrix_C[j] = 0;
        }

        printf("Multiplying matrices using %d process%s\n", i,
               (i > 1) ? "es" : "");

        // Start timing
        struct timeval start, end;
        gettimeofday(&start, 0);

        // Record the pid of each child process
        pid_t *pids = (pid_t *)malloc(i * sizeof(pid_t));
        if (pids == NULL) {
            perror("malloc for pids failed");
            exit(1);
        }

        for (int j = 0; j < i; j++) {
            pids[j] = fork();
            if (pids[j] < 0) {
                perror("fork failed");
                exit(1);
            } else if (pids[j] == 0) { // Child process
                // Each child process computes a portion of the result matrix C
                int start_row = j * dim / i;
                int end_row = (j + 1) * dim / i;

                // Perform matrix multiplication
                /*
                1. r: row of matrix A, C
                2. c: column of matrix B, C
                3. k: column of matrix A, row of matrix B
                4. C[r][c] = sum(A[r][k] * B[k][c]) for k = 0 to dim-1
                */
                for (int r = start_row; r < end_row; r++) {
                    for (int c = 0; c < dim; c++) {
                        unsigned int sum = 0;
                        for (int k = 0; k < dim; k++) {
                            sum +=
                                matrix_AB[r * dim + k] * matrix_AB[k * dim + c];
                        }
                        matrix_C[r * dim + c] = sum;
                    }
                }

                // Child process done, detach shared memory and exit
                shmdt(matrix_C);

                free(matrix_AB);

                exit(0);
            }
        }

        // Parent process waits for all child processes to finish
        for (int j = 0; j < i; j++) {
            waitpid(pids[j], NULL, 0);
        }

        unsigned int checksum = 0;
        checksum = getMatrixChecksum(matrix_C, dim);

        gettimeofday(&end, 0); // End timing
        int sec = end.tv_sec - start.tv_sec;
        int usec = end.tv_usec - start.tv_usec;
        printf("Elapsed time: %f sec, Checksum: %u\n", sec + (usec / 1000000.0),
               checksum);

        free(pids);
    }

    // Detach and remove shared memory segment
    shmdt(matrix_C);
    shmctl(shmid, IPC_RMID, NULL);

    // Free allocated memory
    free(matrix_AB);

    return 0;
}