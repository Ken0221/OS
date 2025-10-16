#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CMD_LEN 1024
#define MAX_ARGS 64

// Signal handler to reap zombie processes from background jobs.
void sigchld_handler(int sig) {
    // WNOHANG ensures the parent doesn't block if there are running children.
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/**
 * @brief Parses the command line input.
 * @param cmd The raw command string.
 * @param args Array to store arguments for the first command.
 * @param infile Pointer to store the input redirection filename.
 * @param outfile Pointer to store the output redirection filename.
 * @param pipe_args Array to store arguments for the command after the pipe.
 * @param is_background Flag to indicate if the command should run in the
 * background.
 * @param has_pipe Flag to indicate if the command contains a pipe.
 */
void parse_command(char* cmd, char** args, char** infile, char** outfile,
                   char** pipe_args, int* is_background, int* has_pipe) {
    int i = 0;
    int arg_idx = 0;
    char** current_args = args;

    *infile = NULL;
    *outfile = NULL;
    *is_background = 0;
    *has_pipe = 0;

    // Clean args arrays
    for (i = 0; i < MAX_ARGS; ++i) {
        args[i] = NULL;
        pipe_args[i] = NULL;
    }

    char* token = strtok(cmd, " \t\n\r");
    while (token != NULL) {
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " \t\n\r");
            if (token != NULL) *infile = token;
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, " \t\n\r");
            if (token != NULL) *outfile = token;
        } else if (strcmp(token, "|") == 0) {
            *has_pipe = 1;
            current_args[arg_idx] = NULL; // Terminate the first command's args
            current_args = pipe_args;     // Switch to filling pipe_args
            arg_idx = 0;                  // Reset index for the new command
        } else if (strcmp(token, "&") == 0) {
            *is_background = 1;
        } else {
            current_args[arg_idx++] = token;
        }
        token = strtok(NULL, " \t\n\r");
    }
    current_args[arg_idx] = NULL; // Terminate the last command's args
}

/**
 * @brief Handles I/O redirection for the current process.
 */
void handle_redirection(char* infile, char* outfile) {
    if (infile) {
        int fd_in = open(infile, O_RDONLY);
        if (fd_in < 0) {
            perror("open input file failed");
            exit(EXIT_FAILURE);
        }
        dup2(fd_in, STDIN_FILENO);
        close(fd_in);
    }
    if (outfile) {
        int fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0) {
            perror("open output file failed");
            exit(EXIT_FAILURE);
        }
        dup2(fd_out, STDOUT_FILENO);
        close(fd_out);
    }
}

/**
 * @brief Executes a single command, which might include redirection.
 */
void execute_single_command(char** args, char* infile, char* outfile,
                            int is_background) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        return;
    }

    if (pid == 0) { // Child process
        handle_redirection(infile, outfile);
        if (execvp(args[0], args) < 0) {
            fprintf(stderr, "Command not found: %s\n", args[0]);
            exit(EXIT_FAILURE);
        }
    } else { // Parent process
        if (!is_background) {
            waitpid(pid, NULL, 0); // Wait for foreground process
        } else {
            printf("Process [%d] started in background.\n", pid);
            // Don't wait, SIGCHLD handler will reap the child
        }
    }
}

/**
 * @brief Executes a command with a pipe (e.g., cmd1 | cmd2).
 */
void execute_pipe_command(char** args1, char** args2) {
    int pipefd[2];
    pid_t pid1, pid2;

    if (pipe(pipefd) < 0) {
        perror("pipe failed");
        return;
    }

    pid1 = fork();
    if (pid1 < 0) {
        perror("fork failed");
        return;
    }

    if (pid1 == 0) {      // Child 1: executes the first command
        close(pipefd[0]); // Close unused read end
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (execvp(args1[0], args1) < 0) {
            fprintf(stderr, "Command not found: %s\n", args1[0]);
            exit(EXIT_FAILURE);
        }
    }

    pid2 = fork();
    if (pid2 < 0) {
        perror("fork failed");
        return;
    }

    if (pid2 == 0) {      // Child 2: executes the second command
        close(pipefd[1]); // Close unused write end
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        if (execvp(args2[0], args2) < 0) {
            fprintf(stderr, "Command not found: %s\n", args2[0]);
            exit(EXIT_FAILURE);
        }
    }

    // Parent process closes both ends of the pipe and waits for children
    close(pipefd[0]);
    close(pipefd[1]);
    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
}

int main() {
    char cmd[MAX_CMD_LEN];
    char* args[MAX_ARGS];
    char* pipe_args[MAX_ARGS];
    char *infile, *outfile;
    int is_background, has_pipe;

    // Register signal handler for SIGCHLD to prevent zombies
    signal(SIGCHLD, sigchld_handler);

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
            break; // Exit on EOF (Ctrl+D)
        }

        if (strcmp(cmd, "\n") == 0) {
            continue; // Skip empty command
        }

        parse_command(cmd, args, &infile, &outfile, pipe_args, &is_background,
                      &has_pipe);

        if (args[0] == NULL) {
            continue;
        }

        if (strcmp(args[0], "exit") == 0) {
            break;
        }

        // --- Execution Logic ---
        if (has_pipe) {
            execute_pipe_command(args, pipe_args);
        } else {
            execute_single_command(args, infile, outfile, is_background);
        }
    }

    printf("\nShell exiting.\n");
    return 0;
}