#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int read_line(int fd, char* word) {
    *word = '\0';
    char buf[2] = "";
    int not_eof;

    while ((not_eof = read(fd, buf, 1)) && *buf != '\n') {
        strcat(word, buf);
    }

    return not_eof;
}

int main() {
    char* ptr[1000] = {NULL};
    int fd = open("test1.txt", O_RDONLY, 0777);
    char line[128];
    int id, size, not_eof;

    do {
        not_eof = read_line(fd, line);

        if (sscanf(line, "A\t%d\t%d", &id, &size)) {
            // debug output
            // char buffer[100];
            // sprintf(buffer, "malloc id = %d Size = %d\n", id, size);

            // write(STDOUT_FILENO, buffer, strlen(buffer));

            ptr[id] = malloc(size);
            for (int i = 0; i < size; ++i) {
                ptr[id][i] = rand();
            }
        } else if (sscanf(line, "D\t%d", &id)) {
            // char dbuffer[100];
            // sprintf(dbuffer, "free id = %d\n", id);

            // write(STDOUT_FILENO, dbuffer, strlen(dbuffer));
            free(ptr[id]);
        }

    } while (not_eof);

    malloc(0);
    return 0;
}