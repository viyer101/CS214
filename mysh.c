#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define MAX_INPUT 1024
#define MAX_ARGS 100

void tokenize(char *input, char *args[]) {
    int i = 0;
    char *token = strtok(input, "\t\n");
    while (token != NULL) {
        args[i++] = token;
        token = strtok(NULL, "\t\n");
    }
    args[i] = NULL;
}

char *findpath(char *cmd) {
    static char path[MAX_INPUT];
    char *dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};

    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], cmd);
        if (access(path, X_OK) == 0) {
            return path;
        }
    }
    return NULL;
}

int main(void) {
    char input[MAX_INPUT];
    while (1) {
        write(STDIN_FILENO, "$", 2);
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        char *args[MAX_ARGS];
        tokenize(input, args);
        if (args[0] == NULL) {
            continue;
        }

        if (strcmp(args[0], "exit") == 0) {
            break;
        }

        char *path = NULL;
        if (strchr(args[0], '/')) {
            path = args[0];
        } else {
            path = findpath(args[0]);
        }

        if (path == NULL) {
            printf("Command not found\n");
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            execv(path, args);
            perror("exec failed");
            exit(1);
        } else {
            wait(NULL);
        }
    }
    return 0;
}