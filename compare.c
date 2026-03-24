#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>

#ifndef COMPARE_SUFFIX
#define COMPARE_SUFFIX ".txt"
#endif

#define READ_BUFSIZE 4096

typedef struct WordNode {
    char *word;
    size_t count;
    double freq;
    struct WordNode *next;
} WordNode;

typedef struct FileInfo {
    char *path;
    WordNode *words;
    size_t total_words;
} FileInfo;

typedef struct Comparison {
    const char *path1;
    const char *path2;
    size_t combined_words;
    double jsd;
} Comparison;

typedef struct {
    FileInfo *items;
    size_t size;
    size_t capacity;
} FileVec;

static int had_error = 0;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (p == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (p == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

static int has_suffix(const char *name, const char *suffix) {
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    return nlen >= slen && strcmp(name + (nlen - slen), suffix) == 0;
}

static int is_hidden_name(const char *name) {
    return name[0] == '.';
}

static char *join_path(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int need_slash = (dlen > 0 && dir[dlen - 1] != '/');
    char *out = xmalloc(dlen + need_slash + nlen + 1);
    memcpy(out, dir, dlen);
    if (need_slash) {
        out[dlen] = '/';
        dlen++;
    }
    memcpy(out + dlen, name, nlen + 1);
    return out;
}

static WordNode *find_or_insert_word(WordNode **head, const char *word) {
    WordNode *prev = NULL;
    WordNode *cur = *head;

    while (cur != NULL && strcmp(cur->word, word) < 0) {
        prev = cur;
        cur = cur->next;
    }

    if (cur != NULL && strcmp(cur->word, word) == 0) {
        return cur;
    }

    WordNode *node = xmalloc(sizeof(*node));
    node->word = xstrdup(word);
    node->count = 0;
    node->freq = 0.0;
    node->next = cur;

    if (prev == NULL) {
        *head = node;
    } else {
        prev->next = node;
    }
    return node;
}

static void add_token(WordNode **head, const char *token, size_t *total_words) {
    if (token[0] == '\0') {
        return;
    }
    WordNode *node = find_or_insert_word(head, token);
    node->count++;
    (*total_words)++;
}

static void finalize_frequencies(FileInfo *info) {
    if (info->total_words == 0) {
        return;
    }
    for (WordNode *node = info->words; node != NULL; node = node->next) {
        node->freq = (double) node->count / (double) info->total_words;
    }
}

static void free_word_list(WordNode *head) {
    while (head != NULL) {
        WordNode *next = head->next;
        free(head->word);
        free(head);
        head = next;
    }
}

static void free_filevec(FileVec *vec) {
    if (vec == NULL) {
        return;
    }
    for (size_t i = 0; i < vec->size; i++) {
        free(vec->items[i].path);
        free_word_list(vec->items[i].words);
    }
    free(vec->items);
}

static void filevec_push(FileVec *vec, FileInfo info) {
    if (vec->size == vec->capacity) {
        size_t new_cap = (vec->capacity == 0) ? 8 : vec->capacity * 2;
        vec->items = xrealloc(vec->items, new_cap * sizeof(*vec->items));
        vec->capacity = new_cap;
    }
    vec->items[vec->size++] = info;
}

static int flush_token(WordNode **head,
                       char **token_buf,
                       size_t *token_len,
                       size_t *total_words) {
    if (*token_len == 0) {
        return 0;
    }
    (*token_buf)[*token_len] = '\0';
    add_token(head, *token_buf, total_words);
    *token_len = 0;
    return 0;
}

static int append_word_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        *buf = xrealloc(*buf, new_cap);
        *cap = new_cap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

static FileInfo read_file_wfd(const char *path) {
    FileInfo info;
    info.path = xstrdup(path);
    info.words = NULL;
    info.total_words = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        had_error = 1;
        return info;
    }

    char read_buf[READ_BUFSIZE];
    char *token_buf = NULL;
    size_t token_len = 0;
    size_t token_cap = 0;

    for (;;) {
        ssize_t nread = read(fd, read_buf, sizeof(read_buf));
        if (nread < 0) {
            perror(path);
            had_error = 1;
            free(token_buf);
            close(fd);
            return info;
        }
        if (nread == 0) {
            break;
        }

        for (ssize_t i = 0; i < nread; i++) {
            unsigned char ch = (unsigned char) read_buf[i];
            if (isspace(ch)) {
                flush_token(&info.words, &token_buf, &token_len, &info.total_words);
            } else if (isalnum(ch) || ch == '-') {
                append_word_char(&token_buf, &token_len, &token_cap, (char) tolower(ch));
            } else {
                /* punctuation inside a whitespace-delimited token is ignored */
            }
        }
    }

    flush_token(&info.words, &token_buf, &token_len, &info.total_words);
    free(token_buf);

    if (close(fd) < 0) {
        perror(path);
        had_error = 1;
    }

    finalize_frequencies(&info);
    return info;
}

static int filevec_contains_path(const FileVec *vec, const char *path) {
    for (size_t i = 0; i < vec->size; i++) {
        if (strcmp(vec->items[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void collect_path(FileVec *files, const char *path, int explicit_file);

static void collect_directory(FileVec *files, const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (dir == NULL) {
        perror(dirpath);
        had_error = 1;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (is_hidden_name(name)) {
            continue;
        }

        char *child = join_path(dirpath, name);
        struct stat st;
        if (lstat(child, &st) < 0) {
            perror(child);
            had_error = 1;
            free(child);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_directory(files, child);
        } else if (S_ISREG(st.st_mode) && has_suffix(name, COMPARE_SUFFIX)) {
            if (!filevec_contains_path(files, child)) {
                FileInfo info = read_file_wfd(child);
                filevec_push(files, info);
            }
        }
        free(child);
    }

    if (closedir(dir) < 0) {
        perror(dirpath);
        had_error = 1;
    }
}

static void collect_path(FileVec *files, const char *path, int explicit_file) {
    const char *base = strrchr(path, '/');
    base = (base == NULL) ? path : base + 1;
    if (is_hidden_name(base)) {
        return;
    }

    struct stat st;
    if (lstat(path, &st) < 0) {
        perror(path);
        had_error = 1;
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        collect_directory(files, path);
    } else if (S_ISREG(st.st_mode)) {
        if ((explicit_file || has_suffix(base, COMPARE_SUFFIX)) &&
            !filevec_contains_path(files, path)) {
            FileInfo info = read_file_wfd(path);
            filevec_push(files, info);
        }
    }
}

static double accumulate_kld_term(double fi, double mean) {
    if (fi == 0.0) {
        return 0.0;
    }
    return fi * log2(fi / mean);
}

static double compute_jsd(const FileInfo *a, const FileInfo *b) {
    const WordNode *wa = a->words;
    const WordNode *wb = b->words;
    double kld_a = 0.0;
    double kld_b = 0.0;

    while (wa != NULL || wb != NULL) {
        if (wb == NULL || (wa != NULL && strcmp(wa->word, wb->word) < 0)) {
            double mean = 0.5 * wa->freq;
            kld_a += accumulate_kld_term(wa->freq, mean);
            wa = wa->next;
        } else if (wa == NULL || strcmp(wb->word, wa->word) < 0) {
            double mean = 0.5 * wb->freq;
            kld_b += accumulate_kld_term(wb->freq, mean);
            wb = wb->next;
        } else {
            double mean = 0.5 * (wa->freq + wb->freq);
            kld_a += accumulate_kld_term(wa->freq, mean);
            kld_b += accumulate_kld_term(wb->freq, mean);
            wa = wa->next;
            wb = wb->next;
        }
    }

    return sqrt(0.5 * kld_a + 0.5 * kld_b);
}

static int cmp_comparisons(const void *lhs, const void *rhs) {
    const Comparison *a = lhs;
    const Comparison *b = rhs;

    if (a->combined_words < b->combined_words) return 1;
    if (a->combined_words > b->combined_words) return -1;

    int c1 = strcmp(a->path1, b->path1);
    if (c1 != 0) return c1;
    return strcmp(a->path2, b->path2);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file_or_directory [... ]\n", argv[0]);
        return EXIT_FAILURE;
    }

    FileVec files = {0};
    for (int i = 1; i < argc; i++) {
        collect_path(&files, argv[i], 1);
    }

    if (files.size < 2) {
        fprintf(stderr, "Error: fewer than two files were collected for analysis.\n");
        free_filevec(&files);
        return EXIT_FAILURE;
    }

    size_t n = files.size;
    size_t cmp_count = n * (n - 1) / 2;
    Comparison *comparisons = xmalloc(cmp_count * sizeof(*comparisons));

    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            comparisons[idx].path1 = files.items[i].path;
            comparisons[idx].path2 = files.items[j].path;
            comparisons[idx].combined_words = files.items[i].total_words + files.items[j].total_words;
            comparisons[idx].jsd = compute_jsd(&files.items[i], &files.items[j]);
            idx++;
        }
    }

    qsort(comparisons, cmp_count, sizeof(*comparisons), cmp_comparisons);

    for (size_t i = 0; i < cmp_count; i++) {
        printf("%.5f %s %s\n",
               comparisons[i].jsd,
               comparisons[i].path1,
               comparisons[i].path2);
    }

    free(comparisons);
    free_filevec(&files);
    return had_error ? EXIT_FAILURE : EXIT_SUCCESS;
}