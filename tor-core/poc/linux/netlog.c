/*
 * Linux leak-test shim (LD_PRELOAD): logs every outgoing connect(), every
 * name lookup (getaddrinfo, gethostbyname*) and every UDP datagram to port
 * 53 the process makes, to the file named by $RV_NETLOG. Lines:
 *   connect <ip> <port>
 *   getaddrinfo <name>
 *   dns-sendto <ip> <port>
 * Test-only; never shipped.
 *
 * gcc -shared -fPIC -O2 -o netlog.so netlog.c -ldl
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void logline(const char* fmt, ...) {
    const char* path = getenv("RV_NETLOG");
    if (path == NULL) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    pthread_mutex_lock(&lock);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t ignored = write(fd, line, (size_t)n);
        (void)ignored;
        close(fd);
    }
    pthread_mutex_unlock(&lock);
}

static void describe(const struct sockaddr* addr, char* ip, size_t len, int* port) {
    ip[0] = '\0';
    *port = -1;
    if (addr == NULL) return;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
        inet_ntop(AF_INET, &in->sin_addr, ip, (socklen_t)len);
        *port = ntohs(in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)addr;
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, (socklen_t)len);
        *port = ntohs(in6->sin6_port);
    }
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    static int (*real)(int, const struct sockaddr*, socklen_t) = NULL;
    if (real == NULL) real = dlsym(RTLD_NEXT, "connect");
    char ip[INET6_ADDRSTRLEN];
    int port;
    describe(addr, ip, sizeof ip, &port);
    if (port >= 0) logline("connect %s %d", ip, port);
    return real(fd, addr, len);
}

ssize_t sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, socklen_t len) {
    static ssize_t (*real)(int, const void*, size_t, int, const struct sockaddr*, socklen_t) = NULL;
    if (real == NULL) real = dlsym(RTLD_NEXT, "sendto");
    char ip[INET6_ADDRSTRLEN];
    int port;
    describe(addr, ip, sizeof ip, &port);
    if (port == 53) logline("dns-sendto %s %d", ip, port);
    return real(fd, buf, n, flags, addr, len);
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    static int (*real)(const char*, const char*, const struct addrinfo*, struct addrinfo**) = NULL;
    if (real == NULL) real = dlsym(RTLD_NEXT, "getaddrinfo");
    logline("getaddrinfo %s", node != NULL ? node : "(null)");
    return real(node, service, hints, res);
}

struct hostent* gethostbyname(const char* name) {
    static struct hostent* (*real)(const char*) = NULL;
    if (real == NULL) real = dlsym(RTLD_NEXT, "gethostbyname");
    logline("getaddrinfo %s", name != NULL ? name : "(null)");
    return real(name);
}
