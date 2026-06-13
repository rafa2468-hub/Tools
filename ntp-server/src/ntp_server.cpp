// ntp_server.cpp
//
// Minimal-footprint NTP server (RFC 5905) for embedded / resource-limited
// systems. Answers IPv4 unicast client (mode 3) requests on UDP using the
// host system clock as the time reference. No dynamic memory allocation,
// no threads, single static buffer per request.

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t kNtpUnixEpochDelta = 2208988800ULL; // seconds between 1900 and 1970 epochs
constexpr size_t kNtpPacketSize = 48;
constexpr int kNtpModeClient = 3;
constexpr int kNtpModeServer = 4;

#pragma pack(push, 1)
struct NtpPacket {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint8_t  ref_id[4];
    uint64_t ref_timestamp;
    uint64_t orig_timestamp;
    uint64_t recv_timestamp;
    uint64_t trans_timestamp;
};
#pragma pack(pop)

static_assert(sizeof(NtpPacket) == kNtpPacketSize, "unexpected NTP packet size");

volatile sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

// Current time as a 64-bit NTP timestamp: seconds since 1900 in the high
// 32 bits, binary fraction of a second in the low 32 bits.
uint64_t ntp_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seconds = static_cast<uint64_t>(ts.tv_sec) + kNtpUnixEpochDelta;
    uint64_t fraction = (static_cast<uint64_t>(ts.tv_nsec) << 32) / 1000000000ULL;
    return (seconds << 32) | (fraction & 0xFFFFFFFFULL);
}

struct Config {
    uint16_t port = 123;
    const char* bind_addr = "0.0.0.0";
    uint8_t stratum = 1;
    int8_t precision = -20; // ~1 microsecond, matches clock_gettime resolution on most systems
    uint8_t ref_id[4] = {'L', 'O', 'C', 'L'};
    const char* drop_user = nullptr;
    bool daemonize = false;
};

void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -a <address>   Bind address (default 0.0.0.0)\n"
        "  -p <port>      UDP port (default 123, requires root for <1024)\n"
        "  -s <stratum>   NTP stratum to report, 1-15 (default 1)\n"
        "  -r <refid>     Reference ID: 4-char code (stratum 1) or IPv4\n"
        "                 address of upstream source (stratum > 1).\n"
        "                 Default: LOCL\n"
        "  -u <user>      Drop privileges to this user after binding\n"
        "  -d             Daemonize (fork to background)\n"
        "  -h             Show this help\n",
        prog);
}

bool parse_args(int argc, char** argv, Config* cfg) {
    int opt;
    const char* refid_arg = nullptr;
    while ((opt = getopt(argc, argv, "a:p:s:r:u:dh")) != -1) {
        switch (opt) {
            case 'a':
                cfg->bind_addr = optarg;
                break;
            case 'p':
                cfg->port = static_cast<uint16_t>(atoi(optarg));
                break;
            case 's': {
                int s = atoi(optarg);
                if (s < 1 || s > 15) {
                    fprintf(stderr, "stratum must be between 1 and 15\n");
                    return false;
                }
                cfg->stratum = static_cast<uint8_t>(s);
                break;
            }
            case 'r':
                refid_arg = optarg;
                break;
            case 'u':
                cfg->drop_user = optarg;
                break;
            case 'd':
                cfg->daemonize = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return false;
        }
    }

    if (refid_arg) {
        if (cfg->stratum > 1) {
            // For stratum > 1, RFC 5905 says the reference ID is the IPv4
            // address of the upstream time source.
            struct in_addr addr;
            if (inet_pton(AF_INET, refid_arg, &addr) == 1) {
                memcpy(cfg->ref_id, &addr.s_addr, 4);
                return true;
            }
            fprintf(stderr, "warning: -r value is not a valid IPv4 address; "
                            "treating it as a literal 4-character code\n");
        }
        memset(cfg->ref_id, 0, sizeof(cfg->ref_id));
        size_t len = strnlen(refid_arg, sizeof(cfg->ref_id));
        memcpy(cfg->ref_id, refid_arg, len);
    }

    return true;
}

bool drop_privileges(const char* user) {
    struct passwd* pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "unknown user '%s'\n", user);
        return false;
    }
    if (setgroups(1, &pw->pw_gid) != 0 ||
        setgid(pw->pw_gid) != 0 ||
        setuid(pw->pw_uid) != 0) {
        fprintf(stderr, "failed to drop privileges to '%s': %s\n", user, strerror(errno));
        return false;
    }
    return true;
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid > 0) {
        exit(0);
    }
    setsid();

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, &cfg)) {
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.bind_addr, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address '%s'\n", cfg.bind_addr);
        close(sock);
        return 1;
    }

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    if (cfg.drop_user && !drop_privileges(cfg.drop_user)) {
        close(sock);
        return 1;
    }

    if (cfg.daemonize) {
        daemonize();
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    // Timestamp recorded at startup, used as the "reference timestamp"
    // (last time the local clock was set/synchronized).
    const uint64_t ref_timestamp = ntp_now();

    uint8_t buf[kNtpPacketSize];

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                              reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        // Capture the receive timestamp as early as possible.
        const uint64_t t2 = ntp_now();

        if (static_cast<size_t>(n) < kNtpPacketSize) {
            continue; // malformed/truncated request, ignore
        }

        NtpPacket request;
        memcpy(&request, buf, sizeof(request));

        const uint8_t mode = request.li_vn_mode & 0x07;
        if (mode != kNtpModeClient) {
            continue; // only answer standard client requests
        }

        uint8_t version = (request.li_vn_mode >> 3) & 0x07;
        if (version < 1 || version > 4) {
            version = 4;
        }

        NtpPacket response;
        memset(&response, 0, sizeof(response));
        response.li_vn_mode = static_cast<uint8_t>((version << 3) | kNtpModeServer);
        response.stratum = cfg.stratum;
        response.poll = request.poll;
        response.precision = cfg.precision;
        response.root_delay = 0;
        response.root_dispersion = 0;
        memcpy(response.ref_id, cfg.ref_id, sizeof(response.ref_id));
        response.ref_timestamp = htobe64(ref_timestamp);
        // Echo the client's transmit timestamp back verbatim (bytes are
        // copied as-is, no endian conversion needed).
        response.orig_timestamp = request.trans_timestamp;
        response.recv_timestamp = htobe64(t2);
        response.trans_timestamp = htobe64(ntp_now());

        sendto(sock, &response, sizeof(response), 0,
               reinterpret_cast<struct sockaddr*>(&client_addr), client_len);
    }

    close(sock);
    return 0;
}
