/*
 Compilation:
 gcc 111511141.c -o 111511141.out `pkg-config fuse --cflags --libs`
 */

#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>

// TAR Header constants
#define BLOCK_SIZE 512
#define TAR_FILE "test.tar"

// Data structure to store file metadata in memory
struct TarNode {
    char path[256];         // Full path (relative to root, e.g., "dir1/file.txt")
    char linkname[100];     // Target path for symbolic links
    int size;               // File size
    int type;               // Type flag: '0'=file, '5'=dir, '2'=symlink
    long data_offset;       // Offset in test.tar where data begins
    
    // Metadata for getattr
    mode_t mode;            // Permissions
    uid_t uid;
    gid_t gid;
    time_t mtime;
    
    struct TarNode *next;
};

// Global head of the linked list
struct TarNode *head = NULL;

// --- Helper Functions ---

// Convert Octal string to integer
long octal_to_int(char *oct) {
    long value = 0;
    while (*oct && *oct != ' ') {
        if (*oct < '0' || *oct > '7') break;
        value = (value << 3) | (*oct++ - '0');
    }
    return value;
}

// Add a node to the linked list
void add_node(struct TarNode *node) {
    if (head == NULL) {
        head = node;
    } else {
        struct TarNode *current = head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = node;
    }
}

// Remove trailing slash from path (for directory normalization)
void trim_slash(char *path) {
    int len = strlen(path);
    if (len > 0 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
}

// Parse the TAR file and build the memory structure
void parse_tar_file() {
    FILE *fp = fopen(TAR_FILE, "rb");
    if (!fp) {
        perror("Cannot open test.tar");
        exit(1);
    }

    unsigned char buffer[BLOCK_SIZE];
    long current_offset = 0;

    while (1) {
        size_t read_size = fread(buffer, 1, BLOCK_SIZE, fp);
        if (read_size < BLOCK_SIZE) break; // End of file

        // Check for empty block (end of archive)
        if (buffer[0] == 0) {
            // Usually 2 empty blocks mark the end, just stop if name is empty
            break;
        }

        struct TarNode *node = (struct TarNode *)malloc(sizeof(struct TarNode));
        memset(node, 0, sizeof(struct TarNode));

        // 1. Parse Name (Offset 0, 100 bytes)
        // Note: Real TAR might use prefix (345), but for this assignment name is usually enough
        strncpy(node->path, (char *)buffer, 100);
        
        // 2. Parse Mode (Offset 100, 8 bytes)
        node->mode = octal_to_int((char *)(buffer + 100));

        // 3. Parse UID (Offset 108, 8 bytes)
        node->uid = octal_to_int((char *)(buffer + 108));

        // 4. Parse GID (Offset 116, 8 bytes)
        node->gid = octal_to_int((char *)(buffer + 116));

        // 5. Parse Size (Offset 124, 12 bytes)
        node->size = octal_to_int((char *)(buffer + 124));

        // 6. Parse Mtime (Offset 136, 12 bytes)
        node->mtime = octal_to_int((char *)(buffer + 136));

        // 7. Parse Typeflag (Offset 156, 1 byte)
        node->type = buffer[156];

        // 8. Parse Linkname (Offset 157, 100 bytes) - for symlinks
        if (node->type == '2') {
            strncpy(node->linkname, (char *)(buffer + 157), 100);
        }

        // Calculate Data Offset
        current_offset += BLOCK_SIZE; // Skip header
        node->data_offset = current_offset;

        // Skip data blocks
        int data_blocks = (node->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        fseek(fp, data_blocks * BLOCK_SIZE, SEEK_CUR);
        current_offset += data_blocks * BLOCK_SIZE;

        // Normalize path: remove trailing slash for directories
        trim_slash(node->path);

        node->next = NULL;
        add_node(node);
    }

    fclose(fp);
}

// Find a node by path
struct TarNode *get_node(const char *path) {
    // FUSE paths start with '/', TAR paths usually don't.
    // We skip the leading '/' from FUSE path.
    const char *target = path;
    if (path[0] == '/') target++; // Skip '/'
    
    // If path is root "/", return NULL (handled separately in getattr)
    if (strlen(target) == 0) return NULL;

    struct TarNode *curr = head;
    while (curr) {
        if (strcmp(curr->path, target) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

// --- FUSE Operations ---

static struct fuse_operations op;

int my_getattr(const char *path, struct stat *st) {
    memset(st, 0, sizeof(struct stat));

    // Case 1: Root Directory
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0444; // Read-only directory
        // st->st_nlink = 2;
        st->st_uid = getuid(); // Use current process owner
        st->st_gid = getgid();
        return 0;
    }

    // Case 2: Files inside tar
    struct TarNode *node = get_node(path);
    if (!node) {
        return -ENOENT;
    }

    if (node->type == '5') { // directory
        st->st_mode = S_IFDIR | node->mode;
        // st->st_nlink = 2;
    } else if (node->type == '2') { // symlink
        st->st_mode = S_IFLNK | node->mode;
        // st->st_nlink = 1;
    } else { // regular file
        st->st_mode = S_IFREG | node->mode;
        // st->st_nlink = 1;
    }

    st->st_size = node->size;
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_mtime = node->mtime;

    return 0;
}

int my_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    // FUSE paths start with /
    const char *target_dir = path;
    if (path[0] == '/') target_dir++; // Skip leading slash for comparison
    
    // If we are at root, target_dir is empty string ""

    filler(buffer, ".", NULL, 0);
    filler(buffer, "..", NULL, 0);

    struct TarNode *curr = head;
    while (curr) {
        // Check if file is inside the target_dir
        
        // Logic:
        // If target_dir is "", we look for files with NO slashes (e.g., "file1.txt", "dir1")
        // If target_dir is "dir1", we look for "dir1/file2.txt"
        
        int target_len = strlen(target_dir);
        int is_child = 0;

        // Root directory listing
        if (target_len == 0) {
            // Child must not contain '/', implies it's top level
            if (strchr(curr->path, '/') == NULL) {
                is_child = 1;
            }
        } 
        // Subdirectory listing
        else {
            // Check if curr->path starts with "target_dir/"
            if (strncmp(curr->path, target_dir, target_len) == 0 && 
                curr->path[target_len] == '/') {
                
                // Ensure it is a direct child (no more slashes after the one we just found)
                char *ptr_after_slash = curr->path + target_len + 1;
                if (strchr(ptr_after_slash, '/') == NULL) {
                    is_child = 1;
                }
            }
        }

        if (is_child) {
            // Extract filename from full path
            char *filename = curr->path;
            if (target_len > 0) {
                filename = curr->path + target_len + 1;
            }
            filler(buffer, filename, NULL, 0);
        }

        curr = curr->next;
    }

    return 0;
}

int my_read(const char *path, char *buffer, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void) fi;

    struct TarNode *node = get_node(path);
    if (!node) return -ENOENT;
    
    // Check if trying to read a directory
    if (node->type == '5') return -EISDIR;

    // Adjust size to avoid reading past EOF
    if (offset >= node->size) return 0;
    if (offset + size > node->size) {
        size = node->size - offset;
    }

    FILE *fp = fopen(TAR_FILE, "rb");
    if (!fp) return -EIO;

    // Seek to the exact position: Data Start + Offset
    fseek(fp, node->data_offset + offset, SEEK_SET);
    
    size_t res = fread(buffer, 1, size, fp);
    fclose(fp);

    return res;
}

int my_readlink(const char *path, char *buffer, size_t size) {
    struct TarNode *node = get_node(path);
    if (!node) return -ENOENT;
    
    if (node->type != '2') return -EINVAL; // Not a symlink

    // Copy linkname to buffer, ensure null termination
    strncpy((char *)buffer, node->linkname, size - 1);
    ((char *)buffer)[size - 1] = '\0';

    return 0;
}

int main(int argc, char *argv[])
{
    memset(&op, 0, sizeof(op)); 
    op.getattr = my_getattr;
    op.readdir = my_readdir;
    op.read = my_read;
    op.readlink = my_readlink;
    
    // Parse encryption/tar file
    parse_tar_file();

    return fuse_main(argc, argv, &op, NULL);
}