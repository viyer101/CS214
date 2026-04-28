#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 16
#define INITIAL_CAPACITY 16
#define MAX_NAME_LEN 32
#define MAX_STATUS_LEN 64
#define MAX_USER_MSG_LEN 80
#define MAX_BODY_LEN 99999
#define READ_CHUNK 4096

typedef struct {
    int fd;
    bool logged_in;
    char name[MAX_NAME_LEN + 1];
    char status[MAX_STATUS_LEN + 1];
    char *inbuf;
    size_t inbuf_len;
    size_t inbuf_cap;
} Client;

typedef struct {
    char code[4];
    char **fields;
    size_t field_count;
    size_t raw_body_len;
} ParsedMessage;

typedef enum {
    EXTRACT_INCOMPLETE,
    EXTRACT_COMPLETE,
    EXTRACT_MALFORMED
} ExtractResult;

static Client *clients = NULL;
static size_t client_count = 0;
static size_t client_cap = 0;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void *xrealloc(void *ptr, size_t size) {
    void *out = realloc(ptr, size);
    if (!out) {
        die("out of memory");
    }
    return out;
}


static char *dup_range(const char *start, size_t len) {
    char *s = malloc(len + 1);
    if (!s) {
        die("out of memory");
    }
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int send_protocol_message(int fd, const char *code, size_t field_count, const char *fields[]) {
    size_t body_len = 0;
    for (size_t i = 0; i < field_count; i++) {
        body_len += strlen(fields[i]) + 1;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "1|%s|%zu|", code, body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }

    size_t total_len = (size_t)header_len + body_len;
    char *msg = malloc(total_len + 1);
    if (!msg) {
        return -1;
    }

    memcpy(msg, header, (size_t)header_len);
    size_t pos = (size_t)header_len;

    for (size_t i = 0; i < field_count; i++) {
        size_t flen = strlen(fields[i]);
        memcpy(msg + pos, fields[i], flen);
        pos += flen;
        msg[pos++] = '|';
    }
    msg[pos] = '\0';

    int rc = write_all(fd, msg, pos);
    free(msg);
    return rc;
}

static int send_err_fd(int fd, int code, const char *explanation) {
    char codebuf[16];
    snprintf(codebuf, sizeof(codebuf), "%d", code);
    const char *fields[2] = {codebuf, explanation};
    return send_protocol_message(fd, "ERR", 2, fields);
}

static int send_msg_fd(int fd, const char *sender, const char *recipient, const char *body) {
    const char *fields[3] = {sender, recipient, body};
    return send_protocol_message(fd, "MSG", 3, fields);
}

static void free_parsed_message(ParsedMessage *msg) {
    if (!msg) {
        return;
    }
    for (size_t i = 0; i < msg->field_count; i++) {
        free(msg->fields[i]);
    }
    free(msg->fields);
    msg->fields = NULL;
    msg->field_count = 0;
}

static void remove_client(size_t idx) {
    close(clients[idx].fd);
    free(clients[idx].inbuf);

    if (idx + 1 < client_count) {
        memmove(&clients[idx], &clients[idx + 1], (client_count - idx - 1) * sizeof(Client));
    }
    client_count--;
}

static Client *find_client_by_name(const char *name) {
    for (size_t i = 0; i < client_count; i++) {
        if (clients[i].logged_in && strcmp(clients[i].name, name) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

static bool valid_name_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

static bool valid_ascii_range(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p > 126) {
            return false;
        }
    }
    return true;
}

static bool valid_name(const char *name) {
    size_t len = strlen(name);
    if (len < 1 || len > MAX_NAME_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!valid_name_char((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

static bool valid_status(const char *status) {
    size_t len = strlen(status);
    if (len > MAX_STATUS_LEN) {
        return false;
    }
    return valid_ascii_range(status);
}

static bool valid_user_message(const char *body) {
    size_t len = strlen(body);
    if (len < 1 || len > MAX_USER_MSG_LEN) {
        return false;
    }
    return valid_ascii_range(body);
}

static void ensure_client_capacity(void) {
    if (client_count < client_cap) {
        return;
    }
    client_cap = client_cap ? client_cap * 2 : INITIAL_CAPACITY;
    clients = xrealloc(clients, client_cap * sizeof(Client));
}

static void add_client(int fd) {
    ensure_client_capacity();

    Client c;
    c.fd = fd;
    c.logged_in = false;
    c.name[0] = '\0';
    c.status[0] = '\0';
    c.inbuf_cap = READ_CHUNK;
    c.inbuf_len = 0;
    c.inbuf = malloc(c.inbuf_cap);
    if (!c.inbuf) {
        die("out of memory");
    }

    clients[client_count++] = c;
}

static void append_to_input(Client *c, const char *buf, size_t len) {
    if (c->inbuf_len + len > c->inbuf_cap) {
        size_t new_cap = c->inbuf_cap;
        while (c->inbuf_len + len > new_cap) {
            new_cap *= 2;
        }
        c->inbuf = xrealloc(c->inbuf, new_cap);
        c->inbuf_cap = new_cap;
    }

    memcpy(c->inbuf + c->inbuf_len, buf, len);
    c->inbuf_len += len;
}

static ExtractResult extract_one_message(Client *c, char **out_msg, size_t *out_len) {
    *out_msg = NULL;
    *out_len = 0;

    char *first = memchr(c->inbuf, '|', c->inbuf_len);
    if (!first) {
        return EXTRACT_INCOMPLETE;
    }
    size_t first_idx = (size_t)(first - c->inbuf);

    char *second = memchr(first + 1, '|', c->inbuf_len - first_idx - 1);
    if (!second) {
        return EXTRACT_INCOMPLETE;
    }
    size_t second_idx = (size_t)(second - c->inbuf);

    char *third = memchr(second + 1, '|', c->inbuf_len - second_idx - 1);
    if (!third) {
        return EXTRACT_INCOMPLETE;
    }
    size_t third_idx = (size_t)(third - c->inbuf);

    if (first_idx != 1) {
        return EXTRACT_MALFORMED;
    }

    char lenbuf[16];
    size_t lenlen = third_idx - second_idx - 1;
    if (lenlen == 0 || lenlen >= sizeof(lenbuf)) {
        return EXTRACT_MALFORMED;
    }

    memcpy(lenbuf, c->inbuf + second_idx + 1, lenlen);
    lenbuf[lenlen] = '\0';

    for (size_t i = 0; i < lenlen; i++) {
        if (lenbuf[i] < '0' || lenbuf[i] > '9') {
            return EXTRACT_MALFORMED;
        }
    }

    char *endptr = NULL;
    unsigned long body_len = strtoul(lenbuf, &endptr, 10);
    if (!endptr || *endptr != '\0' || body_len > MAX_BODY_LEN) {
        return EXTRACT_MALFORMED;
    }

    size_t total_len = third_idx + 1 + (size_t)body_len;
    if (c->inbuf_len < total_len) {
        return EXTRACT_INCOMPLETE;
    }

    *out_msg = malloc(total_len + 1);
    if (!*out_msg) {
        die("out of memory");
    }

    memcpy(*out_msg, c->inbuf, total_len);
    (*out_msg)[total_len] = '\0';
    *out_len = total_len;

    memmove(c->inbuf, c->inbuf + total_len, c->inbuf_len - total_len);
    c->inbuf_len -= total_len;

    return EXTRACT_COMPLETE;
}

static bool parse_message(const char *msg, size_t len, ParsedMessage *out) {
    memset(out, 0, sizeof(*out));

    if (len < 8 || msg[len - 1] != '|') {
        return false;
    }

    const char *p1 = strchr(msg, '|');
    if (!p1) {
        return false;
    }
    const char *p2 = strchr(p1 + 1, '|');
    if (!p2) {
        return false;
    }
    const char *p3 = strchr(p2 + 1, '|');
    if (!p3) {
        return false;
    }

    if ((size_t)(p1 - msg) != 1 || msg[0] != '1') {
        return false;
    }
    if ((size_t)(p2 - p1 - 1) != 3) {
        return false;
    }

    memcpy(out->code, p1 + 1, 3);
    out->code[3] = '\0';

    char lenbuf[16];
    size_t len_digits = (size_t)(p3 - p2 - 1);
    if (len_digits == 0 || len_digits >= sizeof(lenbuf)) {
        return false;
    }

    memcpy(lenbuf, p2 + 1, len_digits);
    lenbuf[len_digits] = '\0';

    for (size_t i = 0; i < len_digits; i++) {
        if (lenbuf[i] < '0' || lenbuf[i] > '9') {
            return false;
        }
    }

    char *endptr = NULL;
    unsigned long body_len = strtoul(lenbuf, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        return false;
    }

    const char *body = p3 + 1;
    size_t actual_body_len = len - (size_t)(body - msg);
    if (actual_body_len != (size_t)body_len) {
        return false;
    }
    out->raw_body_len = actual_body_len;

    if (strcmp(out->code, "NAM") == 0 ||
        strcmp(out->code, "SET") == 0 ||
        strcmp(out->code, "WHO") == 0) {

        out->fields = calloc(1, sizeof(char *));
        if (!out->fields) {
            die("out of memory");
        }

        out->fields[0] = dup_range(body, actual_body_len - 1);
        out->field_count = 1;
        return true;
    }

    if (strcmp(out->code, "MSG") == 0) {
        const char *s1 = memchr(body, '|', actual_body_len);
        if (!s1) {
            return false;
        }

        size_t rem1 = actual_body_len - (size_t)(s1 + 1 - body);
        const char *s2 = memchr(s1 + 1, '|', rem1);
        if (!s2) {
            return false;
        }

        out->fields = calloc(3, sizeof(char *));
        if (!out->fields) {
            die("out of memory");
        }

        out->fields[0] = dup_range(body, (size_t)(s1 - body));
        out->fields[1] = dup_range(s1 + 1, (size_t)(s2 - (s1 + 1)));
        out->fields[2] = dup_range(s2 + 1, (size_t)((body + actual_body_len - 1) - (s2 + 1)));
        out->field_count = 3;
        return true;
    }

    return false;
}

static void disconnect_with_err(size_t idx, int err_code, const char *reason) {
    send_err_fd(clients[idx].fd, err_code, reason);
    remove_client(idx);
}

static void send_to_all_logged_in(const char *sender, const char *recipient, const char *body) {
    for (size_t i = 0; i < client_count; i++) {
        Client *c = &clients[i];
        if (!c->logged_in) {
            continue;
        }
        send_msg_fd(c->fd, sender, recipient, body);
    }
}

static void handle_nam(size_t idx, ParsedMessage *msg) {
    Client *c = &clients[idx];
    const char *name = msg->fields[0];

    if (c->logged_in) {
        disconnect_with_err(idx, 0, "Unreadable");
        return;
    }

    if (!valid_name(name)) {
        if (strlen(name) > MAX_NAME_LEN) {
            send_err_fd(c->fd, 4, "Name too long");
        } else {
            send_err_fd(c->fd, 3, "Illegal name");
        }
        return;
    }

    Client *existing = find_client_by_name(name);
    if (existing) {
        send_err_fd(c->fd, 1, "Name in use");
        return;
    }

    c->logged_in = true;
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = '\0';
    c->status[0] = '\0';

    send_msg_fd(c->fd, "#all", c->name, "Welcome to the chat!");
}

static void handle_set(size_t idx, ParsedMessage *msg) {
    Client *c = &clients[idx];
    const char *status = msg->fields[0];

    if (!c->logged_in) {
        disconnect_with_err(idx, 0, "Must send NAM first");
        return;
    }

    if (strlen(status) > MAX_STATUS_LEN) {
        send_err_fd(c->fd, 4, "Status too long");
        return;
    }

    if (!valid_status(status)) {
        send_err_fd(c->fd, 3, "Illegal status");
        return;
    }

    strncpy(c->status, status, sizeof(c->status) - 1);
    c->status[sizeof(c->status) - 1] = '\0';

    if (status[0] != '\0') {
        size_t needed = strlen(c->name) + strlen(c->status) + strlen(" is now \"\"") + 1;
        char *body = malloc(needed);
        if (!body) {
            die("out of memory");
        }
        snprintf(body, needed, "%s is now \"%s\"", c->name, c->status);
        send_to_all_logged_in("#all", "#all", body);
        free(body);
    }
}

static void handle_msg(size_t idx, ParsedMessage *msg) {
    Client *c = &clients[idx];
    const char *recipient = msg->fields[1];
    const char *body = msg->fields[2];

    if (!c->logged_in) {
        disconnect_with_err(idx, 0, "Must send NAM first");
        return;
    }

    if (strlen(body) > MAX_USER_MSG_LEN) {
        send_err_fd(c->fd, 4, "Message too long");
        return;
    }

    if (!valid_user_message(body)) {
        send_err_fd(c->fd, 3, "Illegal message");
        return;
    }

    if (strcmp(recipient, "#all") == 0) {
        send_to_all_logged_in(c->name, "#all", body);
        return;
    }

    if (!valid_name(recipient)) {
        if (strlen(recipient) > MAX_NAME_LEN) {
            send_err_fd(c->fd, 4, "Recipient too long");
        } else {
            send_err_fd(c->fd, 3, "Illegal recipient");
        }
        return;
    }

    Client *target = find_client_by_name(recipient);
    if (!target) {
        send_err_fd(c->fd, 2, "Unknown recipient");
        return;
    }

    send_msg_fd(target->fd, c->name, target->name, body);
}

static char *build_who_all_body(void) {
    size_t needed = 1;

    for (size_t i = 0; i < client_count; i++) {
        if (!clients[i].logged_in) {
            continue;
        }
        needed += strlen(clients[i].name);
        if (clients[i].status[0] != '\0') {
            needed += 2 + strlen(clients[i].status);
        }
        needed += 1;
    }

    char *out = malloc(needed);
    if (!out) {
        die("out of memory");
    }
    out[0] = '\0';

    bool first = true;
    for (size_t i = 0; i < client_count; i++) {
        if (!clients[i].logged_in) {
            continue;
        }

        if (!first) {
            strcat(out, "\n");
        }

        strcat(out, clients[i].name);
        if (clients[i].status[0] != '\0') {
            strcat(out, ": ");
            strcat(out, clients[i].status);
        }

        first = false;
    }

    return out;
}

static void handle_who(size_t idx, ParsedMessage *msg) {
    Client *c = &clients[idx];
    const char *target = msg->fields[0];

    if (!c->logged_in) {
        disconnect_with_err(idx, 0, "Must send NAM first");
        return;
    }

    if (strcmp(target, "#all") == 0) {
        char *body = build_who_all_body();
        send_msg_fd(c->fd, "#all", c->name, body);
        free(body);
        return;
    }

    if (!valid_name(target)) {
        if (strlen(target) > MAX_NAME_LEN) {
            send_err_fd(c->fd, 4, "Recipient too long");
        } else {
            send_err_fd(c->fd, 3, "Illegal recipient");
        }
        return;
    }

    Client *other = find_client_by_name(target);
    if (!other) {
        send_err_fd(c->fd, 2, "Unknown recipient");
        return;
    }

    if (other->status[0] == '\0') {
        send_msg_fd(c->fd, "#all", c->name, "No status");
    } else {
        size_t needed = strlen(other->name) + strlen(other->status) + 3;
        char *body = malloc(needed);
        if (!body) {
            die("out of memory");
        }
        snprintf(body, needed, "%s: %s", other->name, other->status);
        send_msg_fd(c->fd, "#all", c->name, body);
        free(body);
    }
}

static bool handle_parsed_message(size_t idx, ParsedMessage *msg) {
    if (strcmp(msg->code, "NAM") == 0) {
        handle_nam(idx, msg);
        return idx < client_count;
    }
    if (strcmp(msg->code, "SET") == 0) {
        handle_set(idx, msg);
        return idx < client_count;
    }
    if (strcmp(msg->code, "MSG") == 0) {
        handle_msg(idx, msg);
        return idx < client_count;
    }
    if (strcmp(msg->code, "WHO") == 0) {
        handle_who(idx, msg);
        return idx < client_count;
    }

    disconnect_with_err(idx, 0, "Unreadable");
    return false;
}

static int create_listener(const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        die("getaddrinfo: %s", gai_strerror(rc));
    }

    int listener = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listener < 0) {
            continue;
        }

        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listener, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(listener, BACKLOG) == 0) {
            break;
        }

        close(listener);
        listener = -1;
    }

    freeaddrinfo(res);

    if (listener < 0) {
        die("could not bind/listen on port %s", port);
    }

    return listener;
}

static void accept_new_client(int listener_fd) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    int fd = accept(listener_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) {
        return;
    }

    add_client(fd);
}

static void service_client(size_t idx) {
    Client *c = &clients[idx];
    char buf[READ_CHUNK];

    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        remove_client(idx);
        return;
    }

    append_to_input(c, buf, (size_t)n);

    while (idx < client_count) {
        char *raw = NULL;
        size_t raw_len = 0;

        ExtractResult er = extract_one_message(c, &raw, &raw_len);
        if (er == EXTRACT_INCOMPLETE) {
            break;
        }
        if (er == EXTRACT_MALFORMED) {
            disconnect_with_err(idx, 0, "Unreadable");
            return;
        }

        ParsedMessage msg;
        bool ok = parse_message(raw, raw_len, &msg);
        free(raw);

        if (!ok) {
            free_parsed_message(&msg);
            disconnect_with_err(idx, 0, "Unreadable");
            return;
        }

        bool still_present = handle_parsed_message(idx, &msg);
        free_parsed_message(&msg);

        if (!still_present) {
            return;
        }

        c = &clients[idx];
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int listener_fd = create_listener(argv[1]);

    while (1) {
        struct pollfd *pfds = calloc(client_count + 1, sizeof(struct pollfd));
        if (!pfds) {
            die("out of memory");
        }

        pfds[0].fd = listener_fd;
        pfds[0].events = POLLIN;

        for (size_t i = 0; i < client_count; i++) {
            pfds[i + 1].fd = clients[i].fd;
            pfds[i + 1].events = POLLIN | POLLHUP | POLLERR;
        }

        int rc = poll(pfds, client_count + 1, -1);
        if (rc < 0) {
            free(pfds);
            if (errno == EINTR) {
                continue;
            }
            die("poll failed");
        }

        if (pfds[0].revents & POLLIN) {
            accept_new_client(listener_fd);
        }

        for (size_t i = 0; i < client_count;) {
            short revents = pfds[i + 1].revents;

            if (revents & (POLLHUP | POLLERR)) {
                remove_client(i);
                continue;
            }

            if (revents & POLLIN) {
                size_t before = client_count;
                service_client(i);
                if (client_count < before) {
                    continue;
                }
            }

            i++;
        }

        free(pfds);
    }

    close(listener_fd);
    free(clients);
    return EXIT_SUCCESS;
}