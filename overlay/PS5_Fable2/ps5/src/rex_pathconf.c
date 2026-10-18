#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

long ps5_pathconf(const char *path, int name)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    struct stat st;

    if (stat(path, &st) != 0)
        return -1;

    switch (name) {
#ifdef _PC_NAME_MAX
        case _PC_NAME_MAX:
            return NAME_MAX;
#endif

#ifdef _PC_PATH_MAX
        case _PC_PATH_MAX:
            return PATH_MAX;
#endif

#ifdef _PC_NO_TRUNC
        case _PC_NO_TRUNC:
            return 1;
#endif

#ifdef _PC_CHOWN_RESTRICTED
        case _PC_CHOWN_RESTRICTED:
            return 1;
#endif

        default:
            errno = EINVAL;
            return -1;
    }
}
