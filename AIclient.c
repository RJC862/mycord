// mycord client - reference implementation
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ─── Protocol ────────────────────────────────────────────────────────────────

typedef enum {
    LOGIN        = 0,
    LOGOUT       = 1,
    MESSAGE_SEND = 2,
    MESSAGE_RECV = 10,
    DISCONNECT   = 12,
    SYSTEM       = 13,
} msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t timestamp;
    char     username[32];
    char     message[1024];
} message_t;

// ─── Settings ────────────────────────────────────────────────────────────────

typedef struct {
    struct sockaddr_in server;
    char     username[32];
    bool     quiet;
    volatile bool     running;
    bool     disconnected;
} settings_t;

// ─── Globals ─────────────────────────────────────────────────────────────────

static settings_t        g = {0};
static volatile sig_atomic_t g_signal = 0;

static const char *RED   = "\033[31m";
static const char *GRAY  = "\033[90m";
static const char *RESET = "\033[0m";

// ─── Help ────────────────────────────────────────────────────────────────────

static const char HELP[] =
    "usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet]\n\n"
    "mycord client\n\n"
    "options:\n"
    "  --help                show this help message and exit\n"
    "  --port PORT           port to connect to (default: 8080)\n"
    "  --ip IP               IP to connect to (default: \"127.0.0.1\")\n"
    "  --domain DOMAIN       Domain name to connect to (if domain is specified, IP must not be)\n"
    "  --quiet               do not perform alerts or mention highlighting\n\n"
    "examples:\n"
    "  ./client --help (prints the above message)\n"
    "  ./client --port 1738 (connects to a mycord server at 127.0.0.1:1738)\n"
    "  ./client --domain example.com (connects to a mycord server at example.com:8080)\n";

// ─── IO helpers ──────────────────────────────────────────────────────────────

// Guarantees a full write or returns -1 on error
static ssize_t full_write(int fd, const void *buf, size_t n) {
    size_t      remaining = n;
    const char *pos       = buf;
    while (remaining > 0) {
        ssize_t written = write(fd, pos, remaining);
        if (written < 0) return -1;
        pos       += written;
        remaining -= (size_t)written;
    }
    return (ssize_t)n;
}

// Guarantees a full read; returns 0 on clean close, -1 on error
static ssize_t full_read(int fd, void *buf, size_t n) {
    size_t remaining = n;
    char  *pos       = buf;
    while (remaining > 0) {
        ssize_t r = read(fd, pos, remaining);
        if (r == 0) return 0;   // server closed connection
        if (r <  0) return -1;  // error
        pos       += r;
        remaining -= (size_t)r;
    }
    return (ssize_t)n;
}

// ─── Argument parsing ────────────────────────────────────────────────────────

static int process_args(int argc, char *argv[]) {
    bool ip_set     = false;
    bool domain_set = false;

    // defaults
    g.server.sin_family = AF_INET;
    g.server.sin_port   = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &g.server.sin_addr);

    for (int i = 1; i < argc; i++) {

        if (strcmp(argv[i], "--help") == 0) {
            fputs(HELP, stdout);
            exit(0);

        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing port number\n");
                return -1;
            }
            char *end;
            unsigned long port = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || port > 65535) {
                fprintf(stderr, "Error: invalid port number\n");
                return -1;
            }
            g.server.sin_port = htons((uint16_t)port);

        } else if (strcmp(argv[i], "--ip") == 0) {
            if (domain_set) {
                fprintf(stderr, "Error: cannot specify both --ip and --domain\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing IP address\n");
                return -1;
            }
            if (inet_pton(AF_INET, argv[++i], &g.server.sin_addr) != 1) {
                fprintf(stderr, "Error: invalid IP address\n");
                return -1;
            }
            ip_set = true;

        } else if (strcmp(argv[i], "--domain") == 0) {
            if (ip_set) {
                fprintf(stderr, "Error: cannot specify both --ip and --domain\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing domain name\n");
                return -1;
            }
            struct hostent *h = gethostbyname(argv[++i]);
            if (h == NULL) {
                fprintf(stderr, "Error: could not resolve domain name\n");
                return -1;
            }
            memcpy(&g.server.sin_addr, h->h_addr_list[0], sizeof(struct in_addr));
            domain_set = true;

        } else if (strcmp(argv[i], "--quiet") == 0) {
            g.quiet = true;

        } else {
            fprintf(stderr, "Error: invalid argument: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

// ─── Username ────────────────────────────────────────────────────────────────

static int get_username(void) {
    FILE *fp = popen("whoami", "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: could not run whoami\n");
        return -1;
    }

    if (fgets(g.username, sizeof(g.username), fp) == NULL) {
        fprintf(stderr, "Error: could not read username\n");
        pclose(fp);
        return -1;
    }
    pclose(fp);

    // strip trailing newline
    size_t len = strlen(g.username);
    if (len > 0 && g.username[len - 1] == '\n')
        g.username[--len] = '\0';

    if (len == 0) {
        fprintf(stderr, "Error: username is empty\n");
        return -1;
    }

    // validate: printable ASCII only
    for (size_t i = 0; i < len; i++) {
        if (!isprint((unsigned char)g.username[i])) {
            fprintf(stderr, "Error: username contains non-printable characters\n");
            return -1;
        }
    }
    return 0;
}

// ─── Send helpers ────────────────────────────────────────────────────────────

// Builds and sends a bare message (type only, everything else zeroed)
static int send_type_only(int fd, msg_type_t type) {
    message_t m = {0};
    m.type = htonl((uint32_t)type);
    if (full_write(fd, &m, sizeof(m)) < 0) {
        fprintf(stderr, "Error: failed to send message type %u\n", type);
        return -1;
    }
    return 0;
}

// ─── Receive thread ──────────────────────────────────────────────────────────

static void print_message_recv(const message_t *m) {
    // format timestamp
    char      tbuf[32];
    time_t    t  = (time_t)m->timestamp;
    struct tm tm = {0};
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

    // ensure null-terminated fields (defensive)
    char username[33] = {0};
    char message[1025] = {0};
    memcpy(username, m->username, sizeof(m->username));
    memcpy(message,  m->message,  sizeof(m->message));

    fprintf(stdout, "[%s] %s: ", tbuf, username);

    if (g.quiet) {
        fprintf(stdout, "%s\n", message);
        return;
    }

    // highlight @mentions in red with bell
    char mention[34] = {0}; // '@' + 32 chars + '\0'
    snprintf(mention, sizeof(mention), "@%s", g.username);
    size_t mlen = strlen(mention);

    const char *cursor = message;
    const char *hit;
    bool mentioned = false;

    while ((hit = strstr(cursor, mention)) != NULL) {
        // print everything before the mention
        fprintf(stdout, "%.*s", (int)(hit - cursor), cursor);
        // bell + red mention + reset
        fprintf(stdout, "\a%s%s%s", RED, mention, RESET);
        cursor    = hit + mlen;
        mentioned = true;
    }
    (void)mentioned; // could use to suppress duplicate bells
    // print remainder
    fprintf(stdout, "%s\n", cursor);
}

static void *recv_thread(void *arg) {
    int      fd = (int)(intptr_t)arg;
    message_t m;

    while (g.running) {
        ssize_t r = full_read(fd, &m, sizeof(m));
        if (r <= 0) {
            // connection closed or error
            g.running = false;
            break;
        }

        // convert network → host byte order
        m.type      = ntohl(m.type);
        m.timestamp = ntohl(m.timestamp);

        switch ((msg_type_t)m.type) {
            case MESSAGE_RECV:
                print_message_recv(&m);
                break;

            case SYSTEM:
                fprintf(stdout, "%s[SYSTEM] %s%s\n", GRAY, m.message, RESET);
                break;

            case DISCONNECT:
                fprintf(stdout, "%s[DISCONNECT] %s%s\n", RED, m.message, RESET);
                g.disconnected = true;
                g.running      = false;
                return NULL;

            default:
                fprintf(stderr, "Error: unknown message type %u\n", m.type);
                break;
        }
    }
    return NULL;
}

// ─── Signal handler ──────────────────────────────────────────────────────────

static void handle_signal(int sig) {
    (void)sig;
    g_signal = 1;
}

// ─── Input validation ────────────────────────────────────────────────────────

// Returns true if the message is valid to send
static bool validate_message(const char *msg, ssize_t len) {
    if (len <= 0 || msg[0] == '\0') {
        fprintf(stderr, "Error: empty message\n");
        return false;
    }
    if (len > 1023) {
        fprintf(stderr, "Error: message too long (1-1023 chars)\n");
        return false;
    }
    for (ssize_t i = 0; i < len; i++) {
        if (!isprint((unsigned char)msg[i])) {
            fprintf(stderr, "Error: message contains non-printable ASCII characters\n");
            return false;
        }
    }
    return true;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    // ── signals ──
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // disable stdout buffering so messages appear immediately
    setbuf(stdout, NULL);

    // ── args ──
    if (process_args(argc, argv) < 0)
        return 1;

    // ── username ──
    if (get_username() < 0)
        return 1;

    // ── socket ──
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("Error: could not create socket");
        return 1;
    }

    // ── connect ──
    if (connect(fd, (struct sockaddr *)&g.server, sizeof(g.server)) < 0) {
        perror("Error: could not connect to server");
        close(fd);
        return 1;
    }

    // ── LOGIN ──
    message_t login = {0};
    login.type = htonl(LOGIN);
    strncpy(login.username, g.username, sizeof(login.username) - 1);

    if (full_write(fd, &login, sizeof(login)) < 0) {
        fprintf(stderr, "Error: could not send login message\n");
        close(fd);
        return 1;
    }

    // ── spawn receiver ──
    pthread_t tid;
    g.running = true;

    if (pthread_create(&tid, NULL, recv_thread, (void *)(intptr_t)fd) != 0) {
        fprintf(stderr, "Error: could not create receive thread\n");
        g.running = false;
        close(fd);
        return 1;
    }

    // ── main loop: read stdin and send ──
    char  *line = NULL;
    size_t cap  = 0;

    while (g.running) {
        ssize_t n = getline(&line, &cap, stdin);

        // signal or EOF
        if (g_signal || n < 0) {
            g.running = false;
            break;
        }

        // strip newline
        if (n > 0 && line[n - 1] == '\n')
            line[--n] = '\0';

        if (!validate_message(line, n))
            continue;

        // build and send MESSAGE_SEND
        message_t out = {0};
        out.type = htonl(MESSAGE_SEND);
        strncpy(out.message, line, sizeof(out.message) - 1);

        if (full_write(fd, &out, sizeof(out)) < 0) {
            fprintf(stderr, "Error: could not send message\n");
            // non-fatal: keep running
        }
    }

    free(line);

    // ── LOGOUT (only if not DISCONNECT'd) ──
    if (!g.disconnected)
        send_type_only(fd, LOGOUT);

    // unblock recv thread then join
    close(fd);
    pthread_join(tid, NULL);

    return 0;
}
