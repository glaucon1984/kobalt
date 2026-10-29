/* Symbols that GCC 12's libstdc++ archive (built on Debian against glibc 2.36) references but the
 * PocketBook's glibc 2.23 does not have. Linked into every binary built with the GCC 12 toolchain.
 *   __libc_single_threaded: glibc 2.32+ hint; 0 means "assume threads", which is always safe.
 *   getentropy: glibc 2.25+; read from /dev/urandom. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

char __libc_single_threaded = 0;

int getentropy(void *buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, (char *)buffer + done, length - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            close(fd);
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

/* arc4random / arc4random_buf: glibc 2.36+, used by libstdc++ 12's std::random_device. */
#include <stdint.h>

void arc4random_buf(void *buffer, size_t length) {
    while (length > 0) {
        size_t chunk = length > 256 ? 256 : length;
        if (getentropy(buffer, chunk) != 0) {
            /* Entropy is never unavailable on Linux; fall back to a poor value rather than loop forever. */
            unsigned char *p = buffer;
            for (size_t i = 0; i < chunk; ++i)
                p[i] = (unsigned char)(i * 131u + 17u);
        }
        buffer = (char *)buffer + chunk;
        length -= chunk;
    }
}

uint32_t arc4random(void) {
    uint32_t value;
    arc4random_buf(&value, sizeof value);
    return value;
}
