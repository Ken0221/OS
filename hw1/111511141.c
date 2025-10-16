#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

const int MAX_ARGS = 100; // Maximum number of arguments

void sigchld_handler(int sig) { while (waitpid(-1, NULL, WNOHANG) > 0); }

void parse_command(char* cmd, char** args, char** args2, int* is_background,
                   int* too_much_args, char* infile, char* outfile,
                   int* has_pipe) {
    int i = 0;
    *is_background = 0;
    *too_much_args = 0;
    *has_pipe = 0;

    while (*cmd != '\0' && i < MAX_ARGS - 1) {
        // Skip leading whitespace
        while (*cmd == ' ' || *cmd == '\n' || *cmd == '\t' || *cmd == '\r') {
            *cmd++ = '\0'; // Replace whitespace with null terminator and move
                           // to next character
        }

        // Since I use getline to read the command, the command string
        // always ends with a newline character '\n'
        if (*cmd == '\0') {
            break; // End of string
        }

        char* start = cmd;
        if (*cmd == '"' || *cmd == '\'') {
            char quote = *cmd;
            start++;
            cmd++;

            // Move cmd to the closing quote or end of string
            while (*cmd != quote && *cmd != '\0') {
                cmd++;
            }
            *cmd++ = '\0';
        } else {
            while (*cmd != ' ' && *cmd != '\n' && *cmd != '\t' &&
                   *cmd != '\r' && *cmd != '\0') {
                cmd++;
            }
            *cmd++ = '\0';
        }

        args[i++] = start;
    }

    // char* token = strtok(cmd, " \n\t\r");
    // while (token != NULL && i < MAX_ARGS - 1) {
    //     // the last one have to be NULL, so i < MAX_ARGS - 1
    //     args[i++] = token;
    //     token = strtok(NULL, " \n\t\r");
    // }

    args[i] = NULL; // the last one element of args

    if (*cmd != '\0') {
        fprintf(stderr, "too many arguments: limit is %d\n", MAX_ARGS - 1);
        *too_much_args = 1;
    }

    // Remove surrounding quotes from arguments
    for (int k = 0; args[k] != NULL; k++) {
        int len = strlen(args[k]);
        if (len >= 2) {
            // if the argument is surrounded by quotes, "..."
            if (args[k][0] == '"' && args[k][len - 1] == '"') {
                args[k][len - 1] = '\0';
                // replace the last " with string terminator
                args[k] = args[k] + 1;
                // args[k] is a pointer, so this operation can make it point to
                // the next character
            }
            // if the argument is surrounded by single quotes, '...'
            else if (args[k][0] == '\'' && args[k][len - 1] == '\'') {
                args[k][len - 1] = '\0';
                args[k] = args[k] + 1;
            }
        }
    }

    // Check if the last argument is '&'
    if (i > 0 && strcmp(args[i - 1], "&") == 0) {
        // if yes -> is_background = 1
        *is_background = 1;
        args[i - 1] = NULL; // remove '&' from args
    }

    // Handle I/O redirection and pipe
    for (i = 0; args[i] != NULL; i++) {
        // Handle I/O redirection
        if (strcmp(args[i], "<") == 0) {
            if (args[i + 1] != NULL) {
                strcpy(infile, args[i + 1]);
                args[i] = NULL;     // Remove redirection from args
                args[i + 1] = NULL; // Remove filename from args
                break;
            }
        } else if (strcmp(args[i], ">") == 0) {
            if (args[i + 1] != NULL) {
                strcpy(outfile, args[i + 1]);
                args[i] = NULL;     // Remove redirection from args
                args[i + 1] = NULL; // Remove filename from args
                break;
            }
        }

        // Handle pipe
        // if there is a pipe, split args into args and args2
        if (strcmp(args[i], "|") == 0) {
            *has_pipe = 1;
            args[i] = NULL; // Split the commands at the pipe

            // Copy the second command's arguments to args2
            int j = 0;
            i++;
            while (args[i] != NULL && j < MAX_ARGS - 1) {
                args2[j++] = args[i++];
            }
            args2[j] = NULL; // Terminate args2
            break;
        }
    }
};

void single_command(char* cmd, char** args, int is_background, char* infile,
                    char* outfile) {
    pid_t pid; // pid_t is a data type for process IDs
    pid = fork();

    // if fork() fails
    if (pid < 0) {
        perror("fork failed");
        free(cmd);
        exit(EXIT_FAILURE);
    }
    // else if fork() succeeds
    // 4. Have the child process execute the program
    else if (pid == 0) { // when pid == 0, we are in the child process
        // Handle I/O redirection
        int fd;
        if (infile[0] != '\0') {
            fd = open(infile, O_RDONLY);
            if (fd < 0) {
                perror("open input file failed");
                free(cmd);
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        if (outfile[0] != '\0') {
            fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC,
                      0644); // O_WRONLY: write only, O_CREAT: create if not
                             // exist, O_TRUNC: truncate to zero length if
                             // exist 0644: rw-r--r--
            if (fd < 0) {
                perror("open output file failed");
                free(cmd);
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        // Child process
        execvp(args[0], args);

        // if exe() success
        // it does not return and won't execute the code below
        perror("execvp failed");
        free(cmd);
        exit(EXIT_FAILURE); // Exit if execvp fails
    }
    // 5. Wait until the child terminates
    else { // when pid > 0, we are in the parent process
        // Parent process
        if (is_background == 0) {
            waitpid(pid, NULL, 0); // Wait until the child terminates
        }
    }
    free(cmd);
}

void pipe_command(char* cmd, char** args, char** args2, int is_background) {
    is_background = 0;

    pid_t pid1, pid2;
    int fd[2];
    if (pipe(fd) < 0) {
        perror("pipe failed");
        free(cmd);
        exit(EXIT_FAILURE);
    }

    pid1 = fork();
    if (pid1 < 0) {
        perror("fork failed");
        free(cmd);
        exit(EXIT_FAILURE);
    } else if (pid1 == 0) { // Child 1, output to pipe
        close(fd[0]);       // Close unused read end
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);

        execvp(args[0], args);
        perror("execvp failed");
        free(cmd);
        exit(EXIT_FAILURE);
    }

    pid2 = fork();
    if (pid2 < 0) {
        perror("fork failed");
        free(cmd);
        exit(EXIT_FAILURE);
    } else if (pid2 == 0) { // Child 2, input from pipe
        close(fd[1]);       // Close unused write end
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);

        execvp(args2[0], args2);
        perror("execvp failed");
        free(cmd);
        exit(EXIT_FAILURE);
    }

    // only the parent process reach here
    // Parent process
    close(fd[0]);
    close(fd[1]);

    // Wait for child processes
    if (is_background == 0) {
        waitpid(pid1, NULL, 0);
        waitpid(pid2, NULL, 0);
    }
    free(cmd);
}

int main() {
    signal(SIGCHLD, sigchld_handler);

    while (1) {
        // 1. Display the prompt sign “>” and take a string from user
        char* cmd = NULL;
        size_t len = 0;
        ssize_t read;
        char* args[MAX_ARGS];
        char* args2[MAX_ARGS];
        int is_background, too_much_args, has_pipe;
        char infile[256] = "";
        char outfile[256] = "";

        printf(">");

        read = getline(&cmd, &len, stdin);

        if (read == -1 || strcmp(cmd, "exit\n") == 0) {
            printf("\n");
            free(cmd);
            break; // Exit on EOF (Ctrl+D) or "exit" command
        }

        // 2. Parse the string into a program name and arguments
        parse_command(cmd, args, args2, &is_background, &too_much_args, infile,
                      outfile, &has_pipe); // program name is args[0]

        // skip empty command
        if (args[0] == NULL) {
            free(cmd);
            continue;
        }

        // skip command with too many arguments
        if (too_much_args) {
            free(cmd);
            continue;
        }

        // 3. Fork a child process
        if (has_pipe) {
            pipe_command(cmd, args, args2, is_background);
        } else {
            single_command(cmd, args, is_background, infile, outfile);
        }

        // 6. Go to the first step
    }

    return 0;
}