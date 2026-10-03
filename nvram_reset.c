#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>

#define DEFAULT_PORT  "/dev/ttyACM0"
#define TIMEOUT_S     5

static int open_serial(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);

    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    usleep(100000);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

int main(int argc, char *argv[])
{
    const char *port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = argv[++i];
        } else {
            fprintf(stderr, "usage: nvram_reset [--port DEV]\n");
            return 1;
        }
    }

    int fd = open_serial(port);
    if (fd < 0) return 1;

    /* 'X' takes no length or payload; the firmware pulses /RESET for 500 ms. */
    if (write(fd, "X", 1) != 1) {
        fprintf(stderr, "write (cmd): %s\n", strerror(errno));
        close(fd); return 1;
    }

    /* Wait for "Done." */
    char   buf[64];
    size_t len = 0;
    time_t deadline = time(NULL) + TIMEOUT_S;

    while (time(NULL) < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "select: %s\n", strerror(errno));
            break;
        }
        if (r == 0) continue;

        ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "read: %s\n", strerror(errno));
            break;
        }
        len += (size_t)n;
        buf[len] = '\0';

        if (strstr(buf, "Done.")) {
            close(fd);
            printf("Reset.\n");
            return 0;
        }
        if (len == sizeof(buf) - 1) break;
    }

    close(fd);
    fprintf(stderr, "No response from programmer%s%s\n", len ? ": " : ".", len ? buf : "");
    return 1;
}
