#include <stdio.h>    // sprintf
#include <stdlib.h>   // NULL, size_t
#include <string.h>   // strlen
#include <sys/mman.h> // mmap, munmap
#include <unistd.h>   // write

// header, 32 bytes
typedef struct header {
    size_t total_size;        // 整個區塊的大小 (包含header) (8 bytes)
    int is_free;              // 標記此區塊是否空閒 (4 bytes)
    struct header* next_free; // 空閒串列中的下一個 (8 bytes)
    struct header* prev_free; // 空閒串列中的上一個 (8 bytes)
    char padding[4];          // 補到 32 bytes (4 bytes)
} header_t;

// 記憶體池的起始指標
static void* pool_start = NULL;
// 記憶體池總大小 (20,000 bytes)
static const size_t POOL_SIZE = 20000;
// 標頭大小 (32 bytes)
static const size_t HEADER_SIZE = sizeof(header_t);
// 對齊基數 (32 bytes)
static const size_t ALIGNMENT = 32;

// 多層級空閒串列 (11 個層級)
static header_t* free_lists[11];
// 追蹤 11 個層級串列的尾端
static header_t* free_list_tails[11];

// 將大小向上取整至 32 的倍數。
static size_t round_up_to_32(size_t size) {
    if (size == 0) return ALIGNMENT; // 至少分配 32
    size_t remainder = size % ALIGNMENT;
    if (remainder == 0) return size;
    return size + ALIGNMENT - remainder;
}

// 根據資料大小取得其在空閒串列中的層級。
static int get_level(size_t data_size) {
    if (data_size < 32) return 0;
    if (data_size < 64) return 1;
    if (data_size < 128) return 2;
    if (data_size < 256) return 3;
    if (data_size < 512) return 4;
    if (data_size < 1024) return 5;
    if (data_size < 2048) return 6;
    if (data_size < 4096) return 7;
    if (data_size < 8192) return 8;
    if (data_size < 16384) return 9;
    return 10; // >= 16384
}

/**
 * 將一個空閒區塊從其空閒串列中移除。
 * (需要同時更新 head 和 tail 指標)
 */
static void remove_from_free_list(header_t* chunk) {
    if (chunk == NULL) return;

    size_t data_size = chunk->total_size - HEADER_SIZE;
    int level = get_level(data_size);

    // 更新 head 指標
    if (chunk->prev_free) {
        chunk->prev_free->next_free = chunk->next_free;
    } else {
        // 這是串列的頭
        free_lists[level] = chunk->next_free;
    }

    // 更新 tail 指標
    if (chunk->next_free) {
        chunk->next_free->prev_free = chunk->prev_free;
    } else {
        // 這是串列的尾
        free_list_tails[level] = chunk->prev_free;
    }

    chunk->next_free = NULL;
    chunk->prev_free = NULL;
}

/**
 * 將一個空閒區塊加入到對應的空閒串列的尾端。
 */
static void add_to_free_list(header_t* chunk) {
    if (chunk == NULL) return;

    size_t data_size = chunk->total_size - HEADER_SIZE;
    int level = get_level(data_size);

    header_t* tail = free_list_tails[level];
    chunk->next_free = NULL;

    if (tail == NULL) { // 串列是空的
        free_lists[level] = chunk;
        free_list_tails[level] = chunk;
        chunk->prev_free = NULL;
    } else { // 附加到尾端
        tail->next_free = chunk;
        chunk->prev_free = tail;
        free_list_tails[level] = chunk; // 更新 tail 為新的 chunk
    }
}

/**
 * 尋找一個區塊在物理上的前一個區塊。
 * 透過從頭部遍歷並檢查 "next" 是否為目標 chunk。
 */
static header_t* find_prev_phys(header_t* chunk) {
    if (chunk == NULL || chunk == pool_start) return NULL;

    header_t* current = (header_t*)pool_start;
    // 遍歷直到 current 的下一個是 chunk
    while (current != NULL && (void*)current < (void*)pool_start + POOL_SIZE) {
        // 檢查區塊大小是否有效，防止無限迴圈
        if (current->total_size == 0) break;

        header_t* next = (header_t*)((char*)current + current->total_size);
        if (next == chunk) {
            return current;
        }
        // 檢查是否超出邊界
        if ((void*)next >= (void*)pool_start + POOL_SIZE) {
            break;
        }
        current = next;
    }
    return NULL;
}

/**
 * 處理 malloc(0) 的特殊情況。
 * 尋找最大的空閒區塊，輸出結果，並釋放記憶體池。
 */
static void handle_malloc_zero() {
    size_t max_free_data_size = 0;
    header_t* current = (header_t*)pool_start;

    // 遍歷所有物理區塊
    while (current != NULL && (void*)current < (void*)pool_start + POOL_SIZE) {
        if (current->is_free) {
            size_t data_size = current->total_size - HEADER_SIZE;
            if (data_size > max_free_data_size) {
                max_free_data_size = data_size;
            }
        }
        // 檢查區塊大小是否有效，防止無限迴圈
        if (current->total_size == 0) break;

        // 移至下一個物理區塊
        current = (header_t*)((char*)current + current->total_size);
    }

    // 格式化輸出字串
    char buffer[100];
    sprintf(buffer, "Max Free Chunk Size = %zu\n", max_free_data_size);

    // 使用 write 系統呼叫輸出
    write(STDOUT_FILENO, buffer, strlen(buffer));

    // 釋放記憶體池
    munmap(pool_start, POOL_SIZE);
    pool_start = NULL;
}

// 第一次呼叫 malloc 時初始化記憶體池
static void init_pool() {
    // 使用 mmap 分配 20,000 bytes
    pool_start = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);

    if (pool_start == MAP_FAILED) {
        pool_start = NULL;
        return;
    }

    // 初始化空閒串列 (head 和 tail)
    for (int i = 0; i < 11; i++) {
        free_lists[i] = NULL;
        free_list_tails[i] = NULL;
    }

    // 建立第一個 (也是唯一的) 空閒區塊
    header_t* initial_chunk = (header_t*)pool_start;
    initial_chunk->total_size = POOL_SIZE;
    initial_chunk->is_free = 1;

    // 將其加入空閒串列
    add_to_free_list(initial_chunk);
}

void* malloc(size_t size) {
    // malloc(0)
    if (size == 0) {
        if (pool_start != NULL) {
            handle_malloc_zero();
        }
        return NULL;
    }

    // 第一次呼叫 malloc 時，初始化記憶體池
    if (pool_start == NULL) {
        init_pool();
        if (pool_start == NULL) {
            return NULL; // mmap 失敗
        }
    }

    // 1. 計算所需大小
    // 請求的大小必須向上取整至 32 的倍數
    size_t rounded_data_size = round_up_to_32(size);
    size_t required_total_size = rounded_data_size + HEADER_SIZE;

    // 2. 尋找最佳適應 (Best Fit) 區塊
    header_t* best_fit = NULL;
    size_t min_diff = (size_t)-1; // 'unsigned long long' 的最大值
    int start_level = get_level(rounded_data_size);

    // 從 "最適" 層級開始向上搜尋
    for (int i = start_level; i < 11; i++) {
        header_t* current = free_lists[i];
        while (current) {
            if (current->total_size >= required_total_size) {
                size_t diff = current->total_size - required_total_size;
                if (diff < min_diff) {
                    min_diff = diff;
                    best_fit = current;
                }
            }
            current = current->next_free;
        }

        // 如果在當前層級找到了 Best Fit，就可以停止
        if (best_fit != NULL) {
            break;
        }
    }

    // 3. 如果找不到合適的區塊
    if (best_fit == NULL) {
        return NULL; // 記憶體不足
    }

    // 4. 從空閒串列中移除
    remove_from_free_list(best_fit);

    // 5. 檢查是否需要分割
    size_t remaining_size = best_fit->total_size - required_total_size;

    // 剩餘空間必須足夠容納一個標頭和最小對齊的資料
    // (HEADER_SIZE + ALIGNMENT 是一個安全的最小區塊大小)
    if (remaining_size >= (HEADER_SIZE + ALIGNMENT)) {
        // 建立新的空閒區塊 (在原區塊的尾部)
        header_t* new_free_chunk =
            (header_t*)((char*)best_fit + required_total_size);
        new_free_chunk->total_size = remaining_size;
        new_free_chunk->is_free = 1;

        // 將新切出的空閒區塊加回串列
        add_to_free_list(new_free_chunk);

        // 更新原區塊的大小
        best_fit->total_size = required_total_size;
    }
    // else (剩餘空間太小，不分割，全部分配)

    // 6. 標記區塊為 "已使用" 並返回
    best_fit->is_free = 0;

    // 返回資料區的起始位址 (標頭之後)
    return (void*)((char*)best_fit + HEADER_SIZE);
}

void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    // 檢查 ptr 是否在 mmap 範圍內 (簡易的邊界檢查)
    if (ptr < pool_start || ptr >= (void*)((char*)pool_start + POOL_SIZE)) {
        // 試圖 free 不在 heap pool 中的指標
        return;
    }

    // 1. 透過指標找到區塊標頭
    header_t* chunk_to_free = (header_t*)((char*)ptr - HEADER_SIZE);

    // 如果已經是 free 的，就不要再 free 一次
    if (chunk_to_free->is_free) {
        return;
    }

    // 2. 標記為空閒
    chunk_to_free->is_free = 1;

    // 3. 嘗試與 "下一個" 物理區塊合併
    header_t* next_chunk =
        (header_t*)((char*)chunk_to_free + chunk_to_free->total_size);

    // 檢查 next_chunk 是否在記憶體池範圍內
    if ((void*)next_chunk < (void*)((char*)pool_start + POOL_SIZE) &&
        next_chunk->is_free) {
        // 從空閒串列中移除 next_chunk (因為它要被合併了)
        remove_from_free_list(next_chunk);
        // 合併
        chunk_to_free->total_size += next_chunk->total_size;
    }

    // 4. 嘗試與 "上一個" 物理區塊合併
    header_t* prev_chunk = find_prev_phys(chunk_to_free);
    if (prev_chunk != NULL && prev_chunk->is_free) {
        // 從空閒串列中移除 prev_chunk
        remove_from_free_list(prev_chunk);
        // 合併
        prev_chunk->total_size += chunk_to_free->total_size;
        // 更新指標，讓 "prev_chunk" 成為新的要被加入的空閒區塊
        chunk_to_free = prev_chunk;
    }

    // 5. 將最終 (可能已合併) 的空閒區塊加回空閒串列
    add_to_free_list(chunk_to_free);
}