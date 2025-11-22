#include <stdio.h>    // sprintf
#include <stdlib.h>   // NULL, size_t
#include <string.h>   // strlen
#include <sys/mman.h> // mmap, munmap
#include <unistd.h>   // write

// header struct (32 bytes)
typedef struct header {
    size_t total_size; // Total size of the block (including header) (8 bytes)
    struct header* next_free; // Next in free list (8 bytes)
    struct header* prev_free; // Previous in free list (8 bytes)
    int is_free; // Flag indicating if the block is free (4 bytes), 1 for free,
                 // 0 for used
    char padding[4]; // Padding to make 32 bytes (4 bytes)
} header_t;

// Start of memory pool, if it's null -> never do the malloc
void* pool_start = NULL;
// Total size of memory pool (20,000 bytes)
const size_t POOL_SIZE = 20000;
const size_t HEADER_SIZE = sizeof(header_t); // Size of header (32 bytes)
const size_t ALIGNMENT = 32;                 // Alignment size (32 bytes)

// multi-level free lists (11 levels)
header_t* free_lists[11];
header_t* free_list_tails[11]; // To track the tail of each free list

int get_level(size_t data_size) {
    if (data_size < 0 || data_size > POOL_SIZE - HEADER_SIZE)
        return -1; // invalid size
    else if (data_size < 32)
        return 0;
    else if (data_size < 64)
        return 1;
    else if (data_size < 128)
        return 2;
    else if (data_size < 256)
        return 3;
    else if (data_size < 512)
        return 4;
    else if (data_size < 1024)
        return 5;
    else if (data_size < 2048)
        return 6;
    else if (data_size < 4096)
        return 7;
    else if (data_size < 8192)
        return 8;
    else if (data_size < 16384)
        return 9;
    else
        return 10; // >= 16384
}

void add_to_free_list(header_t* chunk) {
    if (chunk == NULL) return;

    size_t data_size = chunk->total_size - HEADER_SIZE;
    int level = get_level(data_size);
    if (level == -1) return; // invalid size

    header_t* tail = free_list_tails[level]; // Get current tail of the level
    chunk->next_free = NULL;

    if (tail == NULL) { // if this level is empty
        free_lists[level] = chunk;
        free_list_tails[level] = chunk;
        chunk->prev_free = NULL;
    } else { // else, append to the tail
        tail->next_free = chunk;
        chunk->prev_free = tail;
        free_list_tails[level] = chunk; // update tail
    }
}

void init_pool() {
    // Use mmap to allocate 20,000 bytes
    pool_start = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);

    if (pool_start == MAP_FAILED) {
        // if allocation fails
        pool_start = NULL;
        return;
    }

    // Initialize free lists (head and tail)
    for (int i = 0; i < 11; i++) {
        free_lists[i] = NULL;
        free_list_tails[i] = NULL;
    }

    // Create the initial free chunk that spans the entire pool
    header_t* initial_chunk = (header_t*)pool_start;
    initial_chunk->total_size = POOL_SIZE; // 20,000 bytes
    initial_chunk->is_free = 1;            // free
    initial_chunk->next_free = NULL;
    initial_chunk->prev_free = NULL;

    // Add to the appropriate free list
    add_to_free_list(initial_chunk);
}

void handle_malloc_zero() {
    size_t max = 0;

    for (int i = 0; i < 11; i++) {
        header_t* current = free_lists[i];
        while (current != NULL) {
            size_t current_size = current->total_size - HEADER_SIZE;
            if (current_size > max) {
                max = current_size;
            }
            current = current->next_free;
        }
    }

    // printf will call "malloc", so we use write syscall directly
    char buffer[50];
    sprintf(buffer, "Max Free Chunk Size = %zu\n", max);

    write(STDOUT_FILENO, buffer, strlen(buffer));

    // release memory pool
    munmap(pool_start, POOL_SIZE);
    pool_start = NULL;
}

size_t round_up_to_32(size_t size) {
    size_t remainder = size % ALIGNMENT;
    if (size == 0) {
        return ALIGNMENT; // At least 32
    } else if (remainder == 0) {
        return size;
    } else {
        return size - remainder + ALIGNMENT;
    }
}

header_t* find_best_fit(size_t size) {
    int level = get_level(size);
    if (level == -1) return NULL; // invalid size

    for (int i = level; i < 11; i++) {
        header_t* current = free_lists[i];
        header_t* best_fit = NULL;
        size_t min_diff = (size_t)-1; // max value of 'unsigned long long'
        while (current != NULL) {
            if (current->total_size - HEADER_SIZE >= size) {
                size_t diff = current->total_size - HEADER_SIZE - size;
                if (diff < min_diff) {
                    min_diff = diff;
                    best_fit = current;
                }
            }
            current = current->next_free;
        }
        if (best_fit != NULL) {
            return best_fit;
        }
    }
    return NULL;
}

void remove_from_free_list(header_t* chunk) {
    if (chunk == NULL) return;

    size_t data_size = chunk->total_size - HEADER_SIZE;
    int level = get_level(data_size);
    if (level == -1) return; // invalid size

    header_t* prev = chunk->prev_free;
    header_t* next = chunk->next_free;

    if (prev != NULL) {
        prev->next_free = next;
    } else {
        free_lists[level] = next;
    }

    if (next != NULL) {
        next->prev_free = prev;
    } else {
        free_list_tails[level] = prev;
    }

    chunk->next_free = NULL;
    chunk->prev_free = NULL;
}

void memory_allocation_state() {
    // For debugging: print the state of memory allocation
    write(STDOUT_FILENO, "Memory Allocation State:\n", 26);
    header_t* current = (header_t*)pool_start;
    while (current != NULL && (void*)current < (void*)pool_start + POOL_SIZE) {
        char buffer[100];
        sprintf(buffer, "Chunk at %p: size=%zu, is_free=%d\n", (void*)current,
                current->total_size, current->is_free);
        write(STDOUT_FILENO, buffer, strlen(buffer));

        if (current->total_size == 0) break;

        current = (header_t*)((char*)current + current->total_size);
    }
}

void* malloc(size_t size) {
    // char hbuffer[100];
    // sprintf(hbuffer,
    //         "size_t size = %zu, int size = %zu, header* size = %zu, char size
    //         "
    //         "= %zu, header size = %zu\n",
    //         sizeof(size_t), sizeof(int), sizeof(header_t*), sizeof(char),
    //         sizeof(header_t));

    // write(STDOUT_FILENO, hbuffer, strlen(hbuffer));

    // malloc(0)
    if (size == 0) {
        if (pool_start != NULL) {
            handle_malloc_zero();
        }
        return NULL;
    }

    // First time malloc is called, initialize memory pool
    if (pool_start == NULL) {
        init_pool();
    }

    // 1. Calculate required size
    size_t rounded_data_size = round_up_to_32(size);

    // 2. Find best
    header_t* best_fit = find_best_fit(rounded_data_size);
    if (best_fit == NULL) {
        return NULL; // Not enough memory
    }

    // 3. Remove from free list
    remove_from_free_list(best_fit);

    // 4. Check if remaining space is enough for a new free chunk
    size_t remaining_size =
        best_fit->total_size - (rounded_data_size + HEADER_SIZE);

    // minimum block size is HEADER_SIZE + 32
    if (remaining_size >= (HEADER_SIZE + ALIGNMENT)) {
        // Create new free chunk (at the end of the original chunk)
        header_t* new_free_chunk =
            (header_t*)((char*)best_fit + rounded_data_size + HEADER_SIZE);

        new_free_chunk->total_size = remaining_size;
        new_free_chunk->is_free = 1;

        // Add new free chunk back to free list
        add_to_free_list(new_free_chunk);

        // Update original chunk size
        best_fit->total_size = rounded_data_size + HEADER_SIZE;
    }

    // 6. Mark as used and return
    best_fit->is_free = 0;

    // For debugging: print memory allocation state
    // memory_allocation_state();

    return (void*)((char*)best_fit + HEADER_SIZE);
}

header_t* find_prev_phys(header_t* chunk) {
    if (chunk == NULL || chunk == pool_start) return NULL;

    header_t* current = (header_t*)pool_start;
    while (current != NULL && (void*)current < (void*)pool_start + POOL_SIZE) {
        if (current->total_size == 0) break;

        header_t* next = (header_t*)((char*)current + current->total_size);
        if (next == chunk) {
            return current;
        }
        if ((void*)next >= (void*)pool_start + POOL_SIZE) {
            break;
        }
        current = next;
    }
    return NULL;
}

void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    // Check if ptr is within mmap range
    if (ptr < pool_start || ptr >= (void*)((char*)pool_start + POOL_SIZE)) {
        return;
    }

    // 1. Get chunk header
    header_t* chunk_to_free = (header_t*)((char*)ptr - HEADER_SIZE);

    // If already free, do nothing
    if (chunk_to_free->is_free) {
        return;
    }

    // 2. Mark as free
    chunk_to_free->is_free = 1;

    // 3. Try to merge with next physical chunk
    header_t* next_chunk =
        (header_t*)((char*)chunk_to_free + chunk_to_free->total_size);

    if ((void*)next_chunk < (void*)((char*)pool_start + POOL_SIZE) &&
        next_chunk->is_free) {
        remove_from_free_list(next_chunk);
        chunk_to_free->total_size += next_chunk->total_size;
    }

    // 4. Try to merge with previous physical chunk
    header_t* prev_chunk = find_prev_phys(chunk_to_free);
    if (prev_chunk != NULL && prev_chunk->is_free) {
        remove_from_free_list(prev_chunk);
        prev_chunk->total_size += chunk_to_free->total_size;
        chunk_to_free = prev_chunk; // Update to the merged chunk
    }

    // 5. Add the (possibly merged) chunk back to free list
    add_to_free_list(chunk_to_free);
}