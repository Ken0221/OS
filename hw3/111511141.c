#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

int job_cnt = 0;

#define MAX_ELEMENTS 1000000
#define NUM_TASKS 15
#define NUM_SORT_TASKS 8
#define MAX_THREADS 8

int *array;
int *temp_array;
int num_elements;

typedef enum { NOT_DISPATCHED, DISPATCHED, COMPLETED } JobStatus;
int progress_done[NUM_TASKS] = {0};

typedef struct {
    int start;
    int mid;
    int end;
    // id = 1 ~ 6 for merge jobs, id = 7 ~ 14 for sort jobs
    int id;
} Job;

typedef struct Node {
    Job job;
    struct Node *next;
} Node;

Node *job_queue_head = NULL;
Node *job_queue_tail = NULL;
int job_count = 0;

pthread_mutex_t queue_mutex;
pthread_mutex_t progress_mutex;

sem_t jobs_available;
sem_t dispatcher_signal;

int read_input_file() {
    FILE *file = fopen("input.txt", "r");
    if (!file) {
        perror("Could not open input.txt");
        return -1;
    }
    fscanf(file, "%d", &num_elements);
    if (num_elements > MAX_ELEMENTS) {
        fprintf(stderr, "Number of elements exceeds maximum limit.\n");
        fclose(file);
        return -1;
    }
    array = (int *)malloc(sizeof(int) * num_elements);
    temp_array = (int *)malloc(sizeof(int) * num_elements);

    for (int i = 0; i < num_elements; i++) {
        fscanf(file, "%d", &array[i]);
    }
    fclose(file);
    return 0;
}

int write_output_file(int num_threads) {
    char filename[20];
    sprintf(filename, "output_%d.txt", num_threads);
    FILE *file = fopen(filename, "w");
    if (!file) {
        perror("Could not open output file");
        return -1;
    }
    for (int i = 0; i < num_elements; i++) {
        fprintf(file, "%d%c", array[i], (i == num_elements - 1) ? ' ' : ' ');
    }
    fclose(file);
    return 0;
}

void add_job(Job job) {
    Node *new_node = (Node *)malloc(sizeof(Node));
    new_node->job = job;
    new_node->next = NULL;

    if (job_queue_tail == NULL) {
        job_queue_head = new_node;
        job_queue_tail = new_node;
    } else {
        job_queue_tail->next = new_node;
        job_queue_tail = new_node;
    }
    job_count++;

    // calculate total number of jobs
    job_cnt++;
}

Job get_job() {
    if (job_queue_head == NULL) {      // No job available
        Job empty_job = {0, 0, 0, -1}; // Invalid job
        return empty_job;
    }

    Node *temp_node = job_queue_head;
    Job job = temp_node->job;
    job_queue_head = job_queue_head->next;
    if (job_queue_head == NULL) {
        job_queue_tail = NULL;
    }
    free(temp_node);
    job_count--;
    return job;
}

void bubble_sort(int start_index, int end_index) {
    for (int i = start_index; i < end_index - 1; i++) {
        for (int j = start_index; j < end_index - 1 - (i - start_index); j++) {
            if (array[j] > array[j + 1]) {
                int temp = array[j];
                array[j] = array[j + 1];
                array[j + 1] = temp;
            }
        }
    }
}

void merge(int start, int mid, int end) {
    memcpy(temp_array + start, array + start, (end - start) * sizeof(int));

    int i = start; // temp_array's start, left part's start
    int j = mid;   // temp_array's mid, right part's start
    int k = start; // array's start

    while (i < mid && j < end) {
        if (temp_array[i] <= temp_array[j]) {
            array[k++] = temp_array[i++];
        } else {
            array[k++] = temp_array[j++];
        }
    }
    while (i < mid) {
        array[k++] = temp_array[i++];
    }
    while (j < end) {
        array[k++] = temp_array[j++];
    }
}

void print_array(int *arr, int size) {
    for (int i = 0; i < size; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
}

void *dispatcher_thread_func(void *arg) {
    // Sorting
    int chunk_size = num_elements / NUM_SORT_TASKS;
    pthread_mutex_lock(&queue_mutex);
    for (int i = 0; i < NUM_SORT_TASKS; i++) {
        Job job;
        job.start = i * chunk_size;
        job.end =
            (i == NUM_SORT_TASKS - 1) ? num_elements : (i + 1) * chunk_size;
        // Because num_elements may not be perfectly divisible by 8
        job.mid = 0;
        job.id = 7 + i; // sorting id = 7 ~ 14

        add_job(job);
        sem_post(&jobs_available);
    }
    pthread_mutex_unlock(&queue_mutex);

    // Merging

    while (progress_done[0] != COMPLETED) {
        // printf("dispatcher_thread(): dispatcher waiting...\n");
        sem_wait(&dispatcher_signal);
        // printf("dispatcher_thread(): dispatcher signaled\n");

        pthread_mutex_lock(&progress_mutex);
        // printf("dispatcher_thread(): progress_mutex locked\n");
        for (int n = 13; n > 0; n -= 2) {
            if (progress_done[n] == COMPLETED &&
                progress_done[n + 1] == COMPLETED &&
                progress_done[(n - 1) / 2] == NOT_DISPATCHED) {
                // printf("dispatcher_thread(): progess_done: ");
                // for (int i = 0; i < NUM_TASKS; i++) {
                //     printf("%d ", progress_done[i]);
                // }
                // printf("\n");

                progress_done[(n - 1) / 2] = DISPATCHED;

                Job job;
                if (n >= 7) {
                    job.start = (n - 7) * chunk_size;
                    job.mid = job.start + chunk_size;
                    job.end =
                        (n == 13) ? num_elements : job.start + 2 * chunk_size;
                } else if (n >= 3) {
                    job.start = (n - 3) * 2 * chunk_size;
                    job.mid = job.start + 2 * chunk_size;
                    job.end =
                        (n == 5) ? num_elements : job.start + 4 * chunk_size;
                } else {
                    job.start = 0;
                    job.mid = 4 * chunk_size;
                    job.end = num_elements;
                }
                job.id = (n - 1) / 2;
                pthread_mutex_lock(&queue_mutex);
                // printf("dispatcher_thread(): queue_mutex locked\n");
                add_job(job);
                pthread_mutex_unlock(&queue_mutex);
                // printf("dispatcher_thread(): queue_mutex unlock\n");
                sem_post(&jobs_available);
                // printf("dispatcher_thread(): dispatcher added job %d\n",
                // job.id);
            }
        }
        pthread_mutex_unlock(&progress_mutex);
        // printf("dispatcher_thread(): progress_mutex unlock\n");
    }
    return NULL;
}

void *worker_thread_func(void *arg) {
    while (1) {
        sem_wait(&jobs_available);
        // when program is ending, main function will set num_elements to 0
        if (num_elements == -1) break;

        pthread_mutex_lock(&queue_mutex);
        if (job_count > 0) {
            Job job = get_job();
            pthread_mutex_unlock(&queue_mutex);

            // pthread_mutex_lock(&progress_mutex);
            // progress_done[job.id] = DISPATCHED;
            // pthread_mutex_unlock(&progress_mutex);

            if (job.id == -1) continue; // Invalid job, skip

            if (job.id >= 7) {
                bubble_sort(job.start, job.end);
            } else {
                merge(job.start, job.mid, job.end);
            }
            pthread_mutex_lock(&progress_mutex);
            progress_done[job.id] = COMPLETED;
            pthread_mutex_unlock(&progress_mutex);
            sem_post(&dispatcher_signal);
        } else {
            pthread_mutex_unlock(&queue_mutex);
        }
    }
    return NULL;
}

int main() {
    for (int n = 1; n <= MAX_THREADS; n++) {
        // Read input file
        if (read_input_file() != 0) return 1;

        // Initialization
        for (int i = 0; i < NUM_TASKS; i++) {
            progress_done[i] = NOT_DISPATCHED;
        }
        job_queue_head = NULL;
        job_queue_tail = NULL;
        job_count = 0;
        job_cnt = 0;

        pthread_mutex_init(&queue_mutex, NULL);
        pthread_mutex_init(&progress_mutex, NULL);
        sem_init(&jobs_available, 0, 0);
        sem_init(&dispatcher_signal, 0, 0);

        // Start timer
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Create threads
        pthread_t dispatcher_thread;
        pthread_t worker_threads[n];

        pthread_create(&dispatcher_thread, NULL, dispatcher_thread_func, NULL);
        for (int i = 0; i < n; i++) {
            pthread_create(&worker_threads[i], NULL, worker_thread_func, NULL);
        }

        // Wait for completion
        pthread_join(dispatcher_thread, NULL);

        // Stop timer and print results
        gettimeofday(&end, NULL);
        double elapsed_time = (end.tv_sec - start.tv_sec) * 1000.0 +
                              (end.tv_usec - start.tv_usec) / 1000.0;

        printf("worker thread #%d, elapsed %f ms\n", n, elapsed_time);

        // printf("Total jobs created: %d\n", job_cnt);

        // Write to file
        write_output_file(n);

        // Check if sorted
        // int sorted = 1;
        // for (int i = 0; i < num_elements - 1; i++) {
        //     if (array[i] > array[i + 1]) {
        //         sorted = 0;
        //         break;
        //     }
        // }
        // if (!sorted) {
        //     fprintf(stderr, "Array is not sorted correctly!\n");
        // } else {
        //     printf("Array is sorted correctly.\n");
        // }

        // Cleanup
        // Signal worker threads to terminate
        num_elements = -1; // Use as a flag
        for (int i = 0; i < n; i++) {
            sem_post(&jobs_available);
        }
        for (int i = 0; i < n; i++) {
            pthread_join(worker_threads[i], NULL);
        }

        free(array);
        free(temp_array);
        pthread_mutex_destroy(&queue_mutex);
        pthread_mutex_destroy(&progress_mutex);
        sem_destroy(&jobs_available);
        sem_destroy(&dispatcher_signal);
    }
    return 0;
}