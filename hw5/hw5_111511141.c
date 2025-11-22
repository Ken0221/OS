#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define HASH_SIZE 131071 // Prime number around max frame number (65536) * 2
#define MAX_REFERENCES 100000001 // max

typedef struct Node {
  unsigned long vpn;   // virtual page number
  int dirty;           // dirty bit
  struct Node *prev;   // LRU's prev
  struct Node *next;   // LRU's next
  struct Node *h_next; // hash table's next

  // CFLRU optimization
  struct Node *aux_prev; // clean/dirty list prev
  struct Node *aux_next; // clean/dirty list next
  int in_region;         // flag: 1 if in clean-first region, 0 otherwise
} Node;

typedef struct {
  Node *head;        // MRU
  Node *tail;        // LRU
  int size;          // current number of frames
  int capacity;      // frame number
  Node **hash_table; // Array of pointers

  // CFLRU optimization
  Node *clean_head;
  Node *clean_tail;
  Node *dirty_head;
  Node *dirty_tail;
  Node *boundary; // First node in the clean-first region (closest to head)
  int window_size;
} Cache;

// Function Prototypes
Cache *init_cache(int capacity);
void free_cache(Cache *cache);
Node *find_in_hash(Cache *cache, unsigned long vpn);
void add_to_hash(Cache *cache, Node *node);
void remove_from_hash(Cache *cache, Node *node);
void move_to_head(Cache *cache, Node *node);
void remove_node(Cache *cache, Node *node);
void add_to_head(Cache *cache, Node *node);
Node *evict_lru(Cache *cache);
Node *evict_cflru(Cache *cache);

// Global stats
unsigned int hits = 0;
unsigned int misses = 0;
unsigned int write_backs = 0;

// Trace buffer
char *trace_types;
unsigned long *trace_addrs;
int trace_count = 0;

Cache *init_cache(int capacity) {
  Cache *cache = (Cache *)malloc(sizeof(Cache));
  cache->capacity = capacity;
  cache->size = 0;
  cache->head = NULL;
  cache->tail = NULL;
  cache->hash_table = (Node **)calloc(HASH_SIZE, sizeof(Node *));

  // CFLRU init
  cache->clean_head = NULL;
  cache->clean_tail = NULL;
  cache->dirty_head = NULL;
  cache->dirty_tail = NULL;
  cache->boundary = NULL;
  cache->window_size = capacity / 4;

  return cache;
}

void free_cache(Cache *cache) {
  Node *current = cache->head;
  while (current) { // free every element in LRU list
    Node *next = current->next;
    free(current);
    current = next;
  }
  // after this while loop, all node will be freed.
  // So there will be nothing in the hash table
  free(cache->hash_table); // free hash_table
  free(cache);             // free cache itself
}

unsigned int hash_func(unsigned long vpn) { return vpn % HASH_SIZE; }

Node *find_in_hash(Cache *cache, unsigned long vpn) {
  unsigned int idx = hash_func(vpn);
  Node *curr = cache->hash_table[idx];
  while (curr) {          // curr is NOT NULL
    if (curr->vpn == vpn) // find it
      return curr;
    curr = curr->h_next; // collision, h_next
  }
  return NULL;
}

void add_to_hash(Cache *cache, Node *node) {
  // hash table is array of link-lists
  unsigned int idx = hash_func(node->vpn);
  node->h_next = cache->hash_table[idx];
  // org hash_table[idx] == NULL
  // just adding to head of that link-list
  cache->hash_table[idx] = node;
}

void remove_from_hash(Cache *cache, Node *node) {
  unsigned int idx = hash_func(node->vpn);
  Node *curr = cache->hash_table[idx];
  Node *prev = NULL;                 // because hash table didn't record prev
  while (curr) {                     // curr is NOT NULL
    if (curr == node) {              // find the node to be removed
      if (prev)                      // if is NOT head of link-list
        prev->h_next = curr->h_next; // prev's h_next = curr's h_next
      else                           // if is head
        cache->hash_table[idx] = curr->h_next; // head = h_next
      return;                                  // break
    }
    prev = curr;
    curr = curr->h_next;
  }
}

void region_add(Cache *cache, Node *node) {
  if (node->in_region)
    return;
  node->in_region = 1;

  if (node->dirty) {
    // Add to dirty list head
    node->aux_next = cache->dirty_head;
    node->aux_prev = NULL;
    if (cache->dirty_head)
      cache->dirty_head->aux_prev = node;
    cache->dirty_head = node;
    if (!cache->dirty_tail)
      cache->dirty_tail = node;
  } else {
    // Add to clean list head
    node->aux_next = cache->clean_head;
    node->aux_prev = NULL;
    if (cache->clean_head)
      cache->clean_head->aux_prev = node;
    cache->clean_head = node;
    if (!cache->clean_tail)
      cache->clean_tail = node;
  }
}

void region_remove(Cache *cache, Node *node) {
  if (!node->in_region) // if not in region, just return
    return;
  node->in_region = 0; // set in_region to 0 -> remove from region

  if (node->dirty) { // remove from dirty list
    if (node->aux_prev)
      node->aux_prev->aux_next = node->aux_next;
    else
      cache->dirty_head = node->aux_next;

    if (node->aux_next)
      node->aux_next->aux_prev = node->aux_prev;
    else
      cache->dirty_tail = node->aux_prev;
  } else { // remove from clean list
    if (node->aux_prev)
      node->aux_prev->aux_next = node->aux_next;
    else
      cache->clean_head = node->aux_next;

    if (node->aux_next)
      node->aux_next->aux_prev = node->aux_prev;
    else
      cache->clean_tail = node->aux_prev;
  }
}

void move_to_head(Cache *cache, Node *node) {
  // LRU list
  if (cache->head == node) // already at head
    return;

  // CFLRU: Handle region changes
  if (node->in_region) {
    region_remove(cache, node);

    // If we remove from region (and size > window)
    // we need to pull one node from working region to clean-first region and
    // become boundary
    if (cache->size > cache->window_size && cache->boundary &&
        cache->boundary->prev) {
      Node *pull = cache->boundary->prev;
      region_add(cache, pull);
      cache->boundary = pull;
    }
  }

  // Detach, remove this node from LRU list
  if (node->prev)                  // there is a prev -> NOT head
    node->prev->next = node->next; // detach prev
  if (node->next)                  // there is a next -> NOT tail
    node->next->prev = node->prev; // detach next
  if (cache->tail == node)         // tail -> update tail
    cache->tail = node->prev;

  // Attach to head, add this node to HEAD of LRU list
  node->next = cache->head;
  node->prev = NULL;
  if (cache->head) // there is a head -> NOT empty
    cache->head->prev = node;
  cache->head = node; // head = node
  if (!cache->tail)   // there is no tail -> empty
    // the head is also the tail
    cache->tail = node;

  // CFLRU: If size <= window, node stays in region (at head)
  if (cache->size <= cache->window_size) {
    // if size <= window means all nodes are in the clean-first region
    // add org node back
    // we can't do nothing even the node won't leave clean-first region
    // because the sorting matters
    region_add(cache, node);
    cache->boundary = cache->head;
  }
}

void add_to_head(Cache *cache, Node *node) {
  node->next = cache->head;
  node->prev = NULL;
  if (cache->head)            // if cache is NOT empty
    cache->head->prev = node; // org head's prev = new node
  cache->head = node;         // cache's new head = new node
  if (!cache->tail)           // if cache is empty
    cache->tail = node;       // only one node(the new one) is in the cache
                              // tail will also be the new node
  cache->size++;              // size += 1

  // CFLRU: Add to region if size <= window
  if (cache->size <= cache->window_size) {
    region_add(cache, node);
    cache->boundary = node;
  }
}

void remove_node(Cache *cache, Node *node) {
  // CFLRU: Ensure removed from region lists (safety for LRU eviction)
  if (node->in_region) { // Although evict_cfrlu() already does this
    // we do this here just in case
    region_remove(cache, node);
  }

  if (node->prev)                  // if the removed one is NOT head
    node->prev->next = node->next; // prev's next = next
  else                             // is head
    cache->head = node->next;      // cache's head = next

  if (node->next)                  // if is NOT tail
    node->next->prev = node->prev; // next's prev = prev
  else                             // is tail
    cache->tail = node->prev;      // tail = prev

  cache->size--; // size -= 1
}

Node *evict_lru(Cache *cache) { return cache->tail; } // LRU evict tail node

Node *evict_cflru(Cache *cache) {
  Node *victim = NULL;

  // 1. Pick victim from clean list tail
  if (cache->clean_tail) {
    victim = cache->clean_tail;
  }
  // 2. If no clean page, pick from dirty list tail
  else if (cache->dirty_tail) {
    victim = cache->dirty_tail;
  }
  // Fallback (should not happen if logic is correct)
  else {
    victim = cache->tail;
  }

  // Remove from region
  region_remove(cache, victim);

  // Pull one into region if we have space above boundary
  // (We are evicting, so size will decrease. But we want to maintain W region
  // size relative to the remaining nodes) Actually, we are about to remove
  // victim from main list. If we pull now, we maintain W nodes in region
  // (excluding victim).
  if (cache->size > cache->window_size && cache->boundary &&
      cache->boundary->prev) {
    Node *pull = cache->boundary->prev;
    // Ensure we don't pull the victim itself (if boundary->prev==victim)
    // although is impossible(??? or just I can't find the case hahaha
    if (pull != victim) {
      region_add(cache, pull);
      cache->boundary = pull;
    }
  }

  return victim;
}

void load_trace(const char *filepath) { // load trace file
  FILE *fp = fopen(filepath, "r");
  if (!fp) {
    perror("Error opening file");
    exit(1);
  }

  trace_types = (char *)malloc(MAX_REFERENCES * sizeof(char));
  trace_addrs = (unsigned long *)malloc(MAX_REFERENCES * sizeof(unsigned long));

  if (!trace_types || !trace_addrs) {
    fprintf(stderr, "Memory allocation failed for trace buffer\n");
    exit(1);
  }

  char line[256];
  while (fgets(line, sizeof(line), fp)) {
    char type;
    unsigned long addr;

    if (sscanf(line, " %c %lx", &type, &addr) != 2) {
      continue;
    }

    if (trace_count >= MAX_REFERENCES) {
      fprintf(stderr, "Trace file too large\n");
      break;
    }

    trace_types[trace_count] = type;
    trace_addrs[trace_count] = addr;
    trace_count++;
  }

  fclose(fp);
}

void run_simulation(const char *policy, int frame_sizes[], int num_sizes,
                    const char *filepath) {
  printf("%s policy:\n", policy);
  printf("Frame\tHit\t\tMiss\t\tPage fault ratio\tWrite back count\n");

  struct timeval start, end;
  gettimeofday(&start, NULL);

  for (int i = 0; i < num_sizes; i++) { // {4096, 8192, 16384, 32768, 65536}
    int capacity = frame_sizes[i];
    Cache *cache = init_cache(capacity);
    hits = 0;
    misses = 0;
    write_backs = 0;

    for (int j = 0; j < trace_count; j++) {
      char type = trace_types[j];
      unsigned long addr = trace_addrs[j];

      unsigned long vpn = addr >> 12; // 4096 bytes per page, virtual page
                                      // number = address / 4096 = addr >> 12
      int is_write = (type == 'W');

      Node *node = find_in_hash(cache, vpn);
      // if vpn is not in hash (cache miss), node == NULL

      if (node) { // if hit (node != NULL)
        hits++;
        move_to_head(cache, node);
        if (is_write) { // if write, set dirty bit
          if (node->dirty == 0) {
            // Status change: Clean -> Dirty
            // If node is in region, we must move it from Clean List to Dirty
            // List!
            if (node->in_region) {
              region_remove(cache, node); // Remove from Clean List
              node->dirty = 1; // We have to set dirty bit AFTER region_remove()
              // because region_remove() will remove node according its org
              // dirty bit
              region_add(cache, node); // Add to Dirty List
            } else {
              node->dirty = 1;
            }
          }
        }
      } else { // if miss (node == NULL)
        misses++;
        if (cache->size == cache->capacity) {
          // if cache is full, evict one node
          Node *victim = NULL;              // victim node to be evicted
          if (strcmp(policy, "LRU") == 0) { // policy
            victim = evict_lru(cache);      // find victim by LRU
          } else if (strcmp(policy, "CFLRU") == 0) {
            victim = evict_cflru(cache); // find victim by CFLRU
          } else {
            perror("Error policy");
            exit(1);
          }

          if (victim->dirty) // if victim node is dirty, it have to write back
                             // -> write_backs++
            write_backs++;

          remove_from_hash(cache, victim); // remove node from hash
          remove_node(cache, victim);      // remove node from LRU list
          free(victim);                    // free memory
        }

        // if cache is not full, just add new node
        // node's element: vpn, dirty, prev, next, h_next
        Node *new_node = (Node *)malloc(sizeof(Node));
        new_node->vpn = vpn;
        // this node is new one, so it's ok to set the dirty bit to is_write
        new_node->dirty = is_write;
        new_node->in_region = 0;
        new_node->aux_prev = NULL;
        new_node->aux_next = NULL;
        // add to head of LRU list (MRU) (initialize prev, next)
        add_to_head(cache, new_node);
        // add to hash table (initialize h_next)
        add_to_hash(cache, new_node);
      }
    }

    double fault_ratio = (double)misses / (hits + misses);

    printf("%d\t%d\t%d\t\t%.10f\t\t%d\n", capacity, hits, misses, fault_ratio,
           write_backs);

    free_cache(cache);
  }

  gettimeofday(&end, NULL);
  double total_time =
      (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
  printf("Total elapsed time %.6f sec\n\n", total_time);
}

int main(int argc, char *argv[]) {
  // argc: number of arguments(including the program name), argv: arguments
  if (argc < 2) {
    // only 1 argument: the program name, no input file path provided
    fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
    return 1;
  }
  char *file_path = argv[1];

  int frame_sizes[] = {4096, 8192, 16384, 32768, 65536}; // frame numbers
  // number of frame_sizes[]
  int num_sizes = sizeof(frame_sizes) / sizeof(frame_sizes[0]);

  load_trace(file_path);

  run_simulation("LRU", frame_sizes, num_sizes, file_path);
  run_simulation("CFLRU", frame_sizes, num_sizes, file_path);

  free(trace_types);
  free(trace_addrs);

  return 0;
}
