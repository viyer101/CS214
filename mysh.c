#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define READ_CHUNK 4096
#define OUT_MODE (S_IRUSR | S_IWUSR | S_IRGRP)

static const char *SEARCH_DIRS[] = {"/usr/local/bin", "/usr/bin", "/bin"};
static const size_t SEARCH_DIR_COUNT = sizeof(SEARCH_DIRS) / sizeof(SEARCH_DIRS[0]);

typedef struct {
    char **items;
    size_t size;
    size_t cap;
} StrVec;

typedef struct {
    char **argv;
    size_t argc;
    char *input_redir;
    char *output_redir;
    bool has_exit;
} Command;

typedef struct {
    Command *cmds;
    size_t count;
} Job;

typedef struct {
    int fd;
    bool eof;
    char buf[READ_CHUNK];
    size_t start;
    size_t end;
    char *pending;
    size_t pending_len;
    size_t pending_cap;
} LineReader;

typedef enum {
    CMDSTATUS_NONE,
    CMDSTATUS_EXITED,
    CMDSTATUS_SIGNALED
} StatusKind;

typedef struct {
    StatusKind kind;
    int value;
} CommandStatus;

static const char *home_dir = NULL;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void strvec_init(StrVec *v) {
    v->items = NULL;
    v->size = 0;
    v->cap = 0;
}

static void strvec_push_owned(StrVec *v, char *s) {
    if (v->size == v->cap) {
        size_t new_cap = (v->cap == 0) ? 8 : v->cap * 2;
        v->items = xrealloc(v->items, new_cap * sizeof(char *));
        v->cap = new_cap;
    }
    v->items[v->size++] = s;
}

static void strvec_push_copy(StrVec *v, const char *s) {
    strvec_push_owned(v, xstrdup(s));
}

static void strvec_free(StrVec *v) {
    if (!v) {
        return;
    }
    for (size_t i = 0; i < v->size; ++i) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = NULL;
    v->size = 0;
    v->cap = 0;
}

static void command_init(Command *cmd) {
    cmd->argv = NULL;
    cmd->argc = 0;
    cmd->input_redir = NULL;
    cmd->output_redir = NULL;
    cmd->has_exit = false;
}

static void command_free(Command *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    free(cmd->input_redir);
    free(cmd->output_redir);
    cmd->argv = NULL;
    cmd->argc = 0;
    cmd->input_redir = NULL;
    cmd->output_redir = NULL;
    cmd->has_exit = false;
}

static void job_free(Job *job) {
    if (!job) {
        return;
    }
    for (size_t i = 0; i < job->count; ++i) {
        command_free(&job->cmds[i]);
    }
    free(job->cmds);
    job->cmds = NULL;
    job->count = 0;
}

static bool is_builtin_name(const char *name) {
    return strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
           strcmp(name, "which") == 0 || strcmp(name, "exit") == 0;
}

static int cmp_strptr(const void *a, const void *b) {
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static bool wildcard_match_basename(const char *pattern, const char *name) {
    const char *star = strchr(pattern, '*');
    if (!star) {
        return strcmp(pattern, name) == 0;
    }

    size_t prefix_len = (size_t)(star - pattern);
    const char *suffix = star + 1;
    size_t suffix_len = strlen(suffix);
    size_t name_len = strlen(name);

    if (prefix_len == 0 && name[0] == '.') {
        return false;
    }
    if (name_len < prefix_len + suffix_len) {
        return false;
    }
    if (strncmp(name, pattern, prefix_len) != 0) {
        return false;
    }
    if (suffix_len > 0 && strcmp(name + name_len - suffix_len, suffix) != 0) {
        return false;
    }
    return true;
}

static void expand_token(const char *token, StrVec *out) {
    const char *star = strchr(token, '*');
    if (!star) {
        strvec_push_copy(out, token);
        return;
    }

    const char *last_slash = strrchr(token, '/');
    if (last_slash && star < last_slash) {
        strvec_push_copy(out, token);
        return;
    }

    char dirpart[PATH_MAX];
    const char *pattern = token;
    if (last_slash) {
        size_t dirlen = (size_t)(last_slash - token);
        if (dirlen == 0) {
            strcpy(dirpart, "/");
        } else {
            if (dirlen >= sizeof(dirpart)) {
                strvec_push_copy(out, token);
                return;
            }
            memcpy(dirpart, token, dirlen);
            dirpart[dirlen] = '\0';
        }
        pattern = last_slash + 1;
    } else {
        strcpy(dirpart, ".");
    }

    DIR *dir = opendir(dirpart);
    if (!dir) {
        strvec_push_copy(out, token);
        return;
    }

    StrVec matches;
    strvec_init(&matches);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (wildcard_match_basename(pattern, ent->d_name)) {
            char full[PATH_MAX];
            if (last_slash) {
                if (strcmp(dirpart, "/") == 0) {
                    snprintf(full, sizeof(full), "/%s", ent->d_name);
                } else {
                    size_t dir_len = strlen(dirpart);
                    size_t name_len = strlen(ent->d_name);
                    size_t need = dir_len + 1 + name_len + 1;
                    if (need >= sizeof(full)) {
                        continue;
                    }
                    memcpy(full, dirpart, dir_len);
                    full[dir_len] = '/';
                    memcpy(full + dir_len + 1, ent->d_name, name_len + 1);
                }
            } else {
                snprintf(full, sizeof(full), "%s", ent->d_name);
            }
            strvec_push_copy(&matches, full);
        }
    }
    closedir(dir);

    if (matches.size == 0) {
        strvec_push_copy(out, token);
    } else {
        qsort(matches.items, matches.size, sizeof(char *), cmp_strptr);
        for (size_t i = 0; i < matches.size; ++i) {
            strvec_push_owned(out, matches.items[i]);
        }
        free(matches.items);
        matches.items = NULL;
    }
}

static bool resolve_program_path(const char *name, char *resolved, size_t size) {
    if (!name || !*name || is_builtin_name(name)) {
        return false;
    }
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            snprintf(resolved, size, "%s", name);
            return true;
        }
        return false;
    }

    for (size_t i = 0; i < SEARCH_DIR_COUNT; ++i) {
        snprintf(resolved, size, "%s/%s", SEARCH_DIRS[i], name);
        if (access(resolved, X_OK) == 0) {
            return true;
        }
    }
    return false;
}

static int builtin_pwd(const Command *cmd) {
    if (cmd->argc != 1) {
        return 1;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        return 1;
    }
    dprintf(STDOUT_FILENO, "%s\n", cwd);
    return 0;
}

static int builtin_cd(const Command *cmd) {
    const char *target = NULL;
    if (cmd->argc > 2) {
        dprintf(STDERR_FILENO, "cd: too many arguments\n");
        return 1;
    }
    if (cmd->argc == 1) {
        target = home_dir ? home_dir : "/";
    } else {
        target = cmd->argv[1];
    }
    if (chdir(target) != 0) {
        dprintf(STDERR_FILENO, "cd: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_which(const Command *cmd) {
    if (cmd->argc != 2) {
        return 1;
    }
    char path[PATH_MAX];
    if (!resolve_program_path(cmd->argv[1], path, sizeof(path))) {
        return 1;
    }
    dprintf(STDOUT_FILENO, "%s\n", path);
    return 0;
}

static int run_builtin(const Command *cmd) {
    if (cmd->argc == 0) {
        return 0;
    }
    if (strcmp(cmd->argv[0], "pwd") == 0) {
        return builtin_pwd(cmd);
    }
    if (strcmp(cmd->argv[0], "cd") == 0) {
        return builtin_cd(cmd);
    }
    if (strcmp(cmd->argv[0], "which") == 0) {
        return builtin_which(cmd);
    }
    if (strcmp(cmd->argv[0], "exit") == 0) {
        return (cmd->argc == 1) ? 0 : 1;
    }
    return 127;
}

static int tokenize_line(const char *line, StrVec *tokens) {
    strvec_init(tokens);
    const char *p = line;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '\0' || *p == '#') {
            break;
        }
        if (*p == '<' || *p == '>' || *p == '|') {
            char tmp[2] = {*p, '\0'};
            strvec_push_copy(tokens, tmp);
            ++p;
            continue;
        }
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != '#') {
            ++p;
        }
        size_t len = (size_t)(p - start);
        char *tok = xmalloc(len + 1);
        memcpy(tok, start, len);
        tok[len] = '\0';
        strvec_push_owned(tokens, tok);
        if (*p == '#') {
            break;
        }
    }
    return 0;
}

static int finalize_command(StrVec *argv_parts, char *in_redir, char *out_redir, bool saw_exit, Job *job) {
    if (argv_parts->size == 0) {
        return -1;
    }

    job->cmds = xrealloc(job->cmds, (job->count + 1) * sizeof(Command));
    Command *cmd = &job->cmds[job->count];
    command_init(cmd);
    cmd->argc = argv_parts->size;
    cmd->argv = xmalloc((cmd->argc + 1) * sizeof(char *));
    for (size_t i = 0; i < argv_parts->size; ++i) {
        cmd->argv[i] = argv_parts->items[i];
    }
    cmd->argv[cmd->argc] = NULL;
    free(argv_parts->items);
    argv_parts->items = NULL;
    argv_parts->size = 0;
    argv_parts->cap = 0;
    cmd->input_redir = in_redir;
    cmd->output_redir = out_redir;
    cmd->has_exit = saw_exit;
    job->count += 1;
    return 0;
}

static int parse_job(const char *line, Job *job) {
    job->cmds = NULL;
    job->count = 0;

    StrVec tokens;
    tokenize_line(line, &tokens);
    if (tokens.size == 0) {
        strvec_free(&tokens);
        return 1; // empty command
    }

    StrVec current_argv;
    strvec_init(&current_argv);
    char *input_redir = NULL;
    char *output_redir = NULL;
    bool expect_in = false;
    bool expect_out = false;
    bool current_has_exit = false;

    for (size_t i = 0; i < tokens.size; ++i) {
        char *tok = tokens.items[i];
        if (expect_in) {
            if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, "|") == 0) {
                goto syntax_error;
            }
            free(input_redir);
            input_redir = xstrdup(tok);
            expect_in = false;
            continue;
        }
        if (expect_out) {
            if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, "|") == 0) {
                goto syntax_error;
            }
            free(output_redir);
            output_redir = xstrdup(tok);
            expect_out = false;
            continue;
        }

        if (strcmp(tok, "<") == 0) {
            if (input_redir != NULL) {
                goto syntax_error;
            }
            expect_in = true;
        } else if (strcmp(tok, ">") == 0) {
            if (output_redir != NULL) {
                goto syntax_error;
            }
            expect_out = true;
        } else if (strcmp(tok, "|") == 0) {
            if (finalize_command(&current_argv, input_redir, output_redir, current_has_exit, job) != 0) {
                goto syntax_error;
            }
            input_redir = NULL;
            output_redir = NULL;
            current_has_exit = false;
        } else {
            expand_token(tok, &current_argv);
            if (current_argv.size == 1 && strcmp(current_argv.items[0], "exit") == 0) {
                current_has_exit = true;
            } else if (current_argv.size > 0) {
                current_has_exit = (strcmp(current_argv.items[0], "exit") == 0);
            }
        }
    }

    if (expect_in || expect_out) {
        goto syntax_error;
    }
    if (finalize_command(&current_argv, input_redir, output_redir, current_has_exit, job) != 0) {
        goto syntax_error;
    }

    strvec_free(&tokens);
    return 0;

syntax_error:
    dprintf(STDERR_FILENO, "mysh: syntax error\n");
    strvec_free(&tokens);
    strvec_free(&current_argv);
    free(input_redir);
    free(output_redir);
    job_free(job);
    return -1;
}

static void print_prompt(void) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "?");
    }
    if (home_dir && strncmp(cwd, home_dir, strlen(home_dir)) == 0 &&
        (cwd[strlen(home_dir)] == '\0' || cwd[strlen(home_dir)] == '/')) {
        if (cwd[strlen(home_dir)] == '\0') {
            dprintf(STDOUT_FILENO, "~$ ");
        } else {
            dprintf(STDOUT_FILENO, "~%s$ ", cwd + strlen(home_dir));
        }
    } else {
        dprintf(STDOUT_FILENO, "%s$ ", cwd);
    }
}

static void report_status(CommandStatus status) {
    if (status.kind == CMDSTATUS_EXITED && status.value != 0) {
        dprintf(STDOUT_FILENO, "Exited with status %d\n", status.value);
    } else if (status.kind == CMDSTATUS_SIGNALED) {
        const char *msg = strsignal(status.value);
        dprintf(STDOUT_FILENO, "Terminated by signal %s\n", msg ? msg : "unknown");
    }
}

static void line_reader_init(LineReader *lr, int fd) {
    lr->fd = fd;
    lr->eof = false;
    lr->start = 0;
    lr->end = 0;
    lr->pending = NULL;
    lr->pending_len = 0;
    lr->pending_cap = 0;
}

static void line_reader_free(LineReader *lr) {
    free(lr->pending);
    lr->pending = NULL;
    lr->pending_len = 0;
    lr->pending_cap = 0;
}

static void pending_append(LineReader *lr, const char *data, size_t len) {
    if (lr->pending_len + len + 1 > lr->pending_cap) {
        size_t new_cap = lr->pending_cap ? lr->pending_cap * 2 : 128;
        while (new_cap < lr->pending_len + len + 1) {
            new_cap *= 2;
        }
        lr->pending = xrealloc(lr->pending, new_cap);
        lr->pending_cap = new_cap;
    }
    memcpy(lr->pending + lr->pending_len, data, len);
    lr->pending_len += len;
    lr->pending[lr->pending_len] = '\0';
}

static int line_reader_next(LineReader *lr, char **line_out) {
    *line_out = NULL;
    while (1) {
        for (size_t i = lr->start; i < lr->end; ++i) {
            if (lr->buf[i] == '\n') {
                pending_append(lr, lr->buf + lr->start, i - lr->start);
                lr->start = i + 1;
                *line_out = lr->pending ? xstrdup(lr->pending) : xstrdup("");
                lr->pending_len = 0;
                if (lr->pending) {
                    lr->pending[0] = '\0';
                }
                return 1;
            }
        }

        if (lr->eof) {
            if (lr->start < lr->end) {
                pending_append(lr, lr->buf + lr->start, lr->end - lr->start);
                lr->start = lr->end;
            }
            if (lr->pending_len > 0) {
                *line_out = xstrdup(lr->pending);
                lr->pending_len = 0;
                if (lr->pending) {
                    lr->pending[0] = '\0';
                }
                return 1;
            }
            return 0;
        }

        if (lr->start == lr->end) {
            lr->start = 0;
            lr->end = 0;
        } else if (lr->start > 0) {
            memmove(lr->buf, lr->buf + lr->start, lr->end - lr->start);
            lr->end -= lr->start;
            lr->start = 0;
        }

        ssize_t nread = read(lr->fd, lr->buf + lr->end, sizeof(lr->buf) - lr->end);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            return -1;
        }
        if (nread == 0) {
            lr->eof = true;
        } else {
            lr->end += (size_t)nread;
        }
    }
}

static int open_input_for_child(const Command *cmd, bool interactive, bool has_pipe_input) {
    if (cmd->input_redir) {
        int fd = open(cmd->input_redir, O_RDONLY);
        if (fd < 0) {
            perror(cmd->input_redir);
        }
        return fd;
    }
    if (!interactive && !has_pipe_input) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            perror("/dev/null");
        }
        return fd;
    }
    return -1;
}

static int open_output_for_child(const Command *cmd) {
    if (!cmd->output_redir) {
        return -1;
    }
    int fd = open(cmd->output_redir, O_WRONLY | O_CREAT | O_TRUNC, OUT_MODE);
    if (fd < 0) {
        perror(cmd->output_redir);
    }
    return fd;
}

static int run_builtin_parent_with_redirection(const Command *cmd, CommandStatus *status) {
    int save_stdin = -1;
    int save_stdout = -1;
    int in_fd = -1;
    int out_fd = -1;

    if (cmd->input_redir) {
        in_fd = open(cmd->input_redir, O_RDONLY);
        if (in_fd < 0) {
            perror(cmd->input_redir);
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }
        save_stdin = dup(STDIN_FILENO);
        if (save_stdin < 0 || dup2(in_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            if (save_stdin >= 0) close(save_stdin);
            close(in_fd);
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }
    }
    if (cmd->output_redir) {
        out_fd = open(cmd->output_redir, O_WRONLY | O_CREAT | O_TRUNC, OUT_MODE);
        if (out_fd < 0) {
            perror(cmd->output_redir);
            if (save_stdin >= 0) {
                dup2(save_stdin, STDIN_FILENO);
                close(save_stdin);
                close(in_fd);
            }
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }
        save_stdout = dup(STDOUT_FILENO);
        if (save_stdout < 0 || dup2(out_fd, STDOUT_FILENO) < 0) {
            perror("dup2");
            if (save_stdin >= 0) {
                dup2(save_stdin, STDIN_FILENO);
                close(save_stdin);
                close(in_fd);
            }
            if (save_stdout >= 0) close(save_stdout);
            close(out_fd);
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }
    }

    int rc = run_builtin(cmd);
    if (save_stdout >= 0) {
        dup2(save_stdout, STDOUT_FILENO);
        close(save_stdout);
        close(out_fd);
    }
    if (save_stdin >= 0) {
        dup2(save_stdin, STDIN_FILENO);
        close(save_stdin);
        close(in_fd);
    }

    status->kind = CMDSTATUS_EXITED;
    status->value = rc;
    return rc;
}

static int execute_pipeline(const Job *job, bool interactive, CommandStatus *status, bool *should_exit) {
    size_t n = job->count;
    pid_t *pids = xmalloc(n * sizeof(pid_t));
    int prev_read_end = -1;
    ssize_t exit_cmd_index = -1;

    for (size_t i = 0; i < n; ++i) {
        int pipefd[2] = {-1, -1};
        bool has_pipe_output = (i + 1 < n);
        bool has_pipe_input = (i > 0);
        if (job->cmds[i].has_exit) {
            exit_cmd_index = (ssize_t)i;
        }

        if (has_pipe_output && pipe(pipefd) < 0) {
            perror("pipe");
            if (prev_read_end >= 0) close(prev_read_end);
            free(pids);
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (prev_read_end >= 0) close(prev_read_end);
            if (pipefd[0] >= 0) close(pipefd[0]);
            if (pipefd[1] >= 0) close(pipefd[1]);
            free(pids);
            status->kind = CMDSTATUS_EXITED;
            status->value = 1;
            return 1;
        }

        if (pid == 0) {
            if (prev_read_end >= 0) {
                if (dup2(prev_read_end, STDIN_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            } else {
                int in_fd = open_input_for_child(&job->cmds[i], interactive, has_pipe_input);
                if (in_fd >= 0) {
                    if (dup2(in_fd, STDIN_FILENO) < 0) {
                        perror("dup2");
                        close(in_fd);
                        _exit(1);
                    }
                    close(in_fd);
                } else if (job->cmds[i].input_redir && in_fd < 0) {
                    _exit(1);
                }
            }

            if (has_pipe_output) {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            } else {
                int out_fd = open_output_for_child(&job->cmds[i]);
                if (out_fd >= 0) {
                    if (dup2(out_fd, STDOUT_FILENO) < 0) {
                        perror("dup2");
                        close(out_fd);
                        _exit(1);
                    }
                    close(out_fd);
                } else if (job->cmds[i].output_redir && out_fd < 0) {
                    _exit(1);
                }
            }

            if (prev_read_end >= 0) close(prev_read_end);
            if (pipefd[0] >= 0) close(pipefd[0]);
            if (pipefd[1] >= 0) close(pipefd[1]);

            const Command *cmd = &job->cmds[i];
            if (cmd->argc == 0) {
                _exit(0);
            }
            if (is_builtin_name(cmd->argv[0])) {
                _exit(run_builtin(cmd));
            }

            char path[PATH_MAX];
            if (!resolve_program_path(cmd->argv[0], path, sizeof(path))) {
                dprintf(STDERR_FILENO, "%s: command not found\n", cmd->argv[0]);
                _exit(127);
            }
            execv(path, cmd->argv);
            perror(cmd->argv[0]);
            _exit(127);
        }

        pids[i] = pid;
        if (prev_read_end >= 0) close(prev_read_end);
        if (pipefd[1] >= 0) close(pipefd[1]);
        prev_read_end = pipefd[0];
    }

    if (prev_read_end >= 0) close(prev_read_end);

    int result = 0;
    int exit_cmd_rc = -1;

    for (size_t i = 0; i < n; ++i) {
        int wstatus = 0;
        if (waitpid(pids[i], &wstatus, 0) < 0) {
            perror("waitpid");
            result = 1;
            continue;
        }

        if ((ssize_t)i == exit_cmd_index) {
            if (WIFEXITED(wstatus)) {
                exit_cmd_rc = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                exit_cmd_rc = 128 + WTERMSIG(wstatus);
            }
        }

        if (i + 1 == n) {
            if (WIFEXITED(wstatus)) {
                status->kind = CMDSTATUS_EXITED;
                status->value = WEXITSTATUS(wstatus);
                result = status->value;
            } else if (WIFSIGNALED(wstatus)) {
                status->kind = CMDSTATUS_SIGNALED;
                status->value = WTERMSIG(wstatus);
                result = 128 + status->value;
            }
        }
    }

    free(pids);

    if (exit_cmd_index >= 0 && exit_cmd_rc == 0) {
        *should_exit = true;
    }

    return result;
}

static int execute_job(const Job *job, bool interactive, CommandStatus *status, bool *should_exit) {
    status->kind = CMDSTATUS_NONE;
    status->value = 0;

    if (job->count == 0) {
        return 0;
    }

    if (job->count == 1 && job->cmds[0].argc > 0 && is_builtin_name(job->cmds[0].argv[0])) {
        const Command *cmd = &job->cmds[0];
        int rc = run_builtin_parent_with_redirection(cmd, status);

        if (strcmp(cmd->argv[0], "exit") == 0 && rc == 0) {
            *should_exit = true;
        }

        return rc;
    }

    return execute_pipeline(job, interactive, status, should_exit);
}

int main(int argc, char **argv) {
    int input_fd = STDIN_FILENO;
    bool interactive = false;

    if (argc > 2) {
        dprintf(STDERR_FILENO, "Usage: %s [script]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        input_fd = open(argv[1], O_RDONLY);
        if (input_fd < 0) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
        interactive = false;
    } else {
        interactive = isatty(STDIN_FILENO);
    }

    home_dir = getenv("HOME");

    if (interactive) {
        dprintf(STDOUT_FILENO, "Welcome to my shell!\n");
    }

    LineReader reader;
    line_reader_init(&reader, input_fd);

    bool done = false;
    CommandStatus last_status = {CMDSTATUS_NONE, 0};

    while (!done) {
        if (interactive) {
            report_status(last_status);
            print_prompt();
        }

        char *line = NULL;
        int rc = line_reader_next(&reader, &line);
        if (rc < 0) {
            free(line);
            break;
        }
        if (rc == 0) {
            break;
        }

        Job job;
        int parse_rc = parse_job(line, &job);
        free(line);

        if (parse_rc > 0) {
            last_status.kind = CMDSTATUS_NONE;
            last_status.value = 0;
            continue;
        }
        if (parse_rc < 0) {
            last_status.kind = CMDSTATUS_EXITED;
            last_status.value = 1;
            continue;
        }

        bool should_exit = false;
        execute_job(&job, interactive, &last_status, &should_exit);
        job_free(&job);
        if (should_exit) {
            done = true;
        }
    }

    if (interactive) {
        dprintf(STDOUT_FILENO, "mysh: exiting\n");
    }

    line_reader_free(&reader);
    if (argc == 2) {
        close(input_fd);
    }
    return EXIT_SUCCESS;
}
