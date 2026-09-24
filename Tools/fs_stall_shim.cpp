/* A recording card that stops answering, on demand, for the real app.
 *
 * A USB card pulled or wedged at the wrong moment does not return an error: the
 * filesystem call that touches it simply never comes back. No fixture on a CI
 * runner behaves like that -- tmpfs, loop devices and full drives all answer,
 * quickly and honestly -- so the message-thread paths that touch the card at
 * Record and Stop were only ever tested against fakes inside one class.
 *
 * This is preloaded into the unmodified app. Every libc call that touches a
 * path under MMA_STALL_PREFIX, or a descriptor opened there, is let through
 * normally until the file MMA_STALL_TRIGGER exists. From then on it blocks --
 * the calling thread sleeps inside the call exactly as it would inside a dead
 * USB mass-storage request -- until the trigger is removed, when it finishes
 * normally. Everything outside the prefix, including the local backup, the
 * settings and the log, is untouched.
 *
 *   MMA_STALL_PREFIX   directory standing in for the card (exact path or below)
 *   MMA_STALL_TRIGGER  while this file exists, the card does not answer
 */
// Fortified builds turn open() and friends into inline wrappers, which would
// collide with the definitions below.
#undef _FORTIFY_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

template <typename Fn>
Fn real (const char* name)
{
    return reinterpret_cast<Fn> (dlsym (RTLD_NEXT, name));
}

const std::string& prefix()
{
    static const std::string p = [] {
        const char* v = std::getenv ("MMA_STALL_PREFIX");
        std::string s = v != nullptr ? v : "";
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }();
    return p;
}

const char* trigger()
{
    static const char* t = std::getenv ("MMA_STALL_TRIGGER");
    return t;
}

bool underPrefix (const char* path)
{
    if (path == nullptr || prefix().empty())
        return false;

    std::string p (path);

    if (! p.empty() && p[0] != '/')
    {
        char cwd[4096];
        if (::getcwd (cwd, sizeof (cwd)) == nullptr)
            return false;
        p = std::string (cwd) + "/" + p;
    }

    // The exact directory, or something inside it. "RECORDINGS-MIRROR" beside
    // "RECORDINGS" is the local backup and must never be caught by this.
    return p == prefix() || p.compare (0, prefix().size() + 1, prefix() + "/") == 0;
}

std::mutex& fdMutex() { static std::mutex m; return m; }
std::set<int>& cardFds() { static std::set<int> s; return s; }

bool isCardFd (int fd)
{
    const std::lock_guard<std::mutex> lock (fdMutex());
    return cardFds().count (fd) != 0;
}

void rememberFd (int fd, const char* path)
{
    if (fd < 0 || ! underPrefix (path))
        return;
    const std::lock_guard<std::mutex> lock (fdMutex());
    cardFds().insert (fd);
}

void forgetFd (int fd)
{
    const std::lock_guard<std::mutex> lock (fdMutex());
    cardFds().erase (fd);
}

bool cardIsDead()
{
    static auto realAccess = real<int (*) (const char*, int)> ("access");
    return trigger() != nullptr && realAccess (trigger(), F_OK) == 0;
}

// The whole point: block here, in the call, for as long as the card is gone.
void waitWhileCardIsDead()
{
    while (cardIsDead())
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
}

void touchPath (const char* path)
{
    if (underPrefix (path))
        waitWhileCardIsDead();
}

void touchFd (int fd)
{
    if (isCardFd (fd))
        waitWhileCardIsDead();
}

mode_t modeArg (int flags, va_list args)
{
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE
               ? static_cast<mode_t> (va_arg (args, int)) : 0;
}

} // namespace

extern "C" {

int open (const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int, ...)> ("open");
    const int fd = fn (path, flags, mode);
    rememberFd (fd, path);
    return fd;
}

int open64 (const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int, ...)> ("open64");
    const int fd = fn (path, flags, mode);
    rememberFd (fd, path);
    return fd;
}

int openat (int dirfd, const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, ...)> ("openat");
    const int fd = fn (dirfd, path, flags, mode);
    rememberFd (fd, path);
    return fd;
}

int openat64 (int dirfd, const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, ...)> ("openat64");
    const int fd = fn (dirfd, path, flags, mode);
    rememberFd (fd, path);
    return fd;
}

int creat (const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, mode_t)> ("creat");
    const int fd = fn (path, mode);
    rememberFd (fd, path);
    return fd;
}

int close (int fd)
{
    touchFd (fd);
    forgetFd (fd);
    static const auto fn = real<int (*) (int)> ("close");
    return fn (fd);
}

ssize_t write (int fd, const void* buf, size_t n)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t)> ("write");
    return fn (fd, buf, n);
}

ssize_t pwrite (int fd, const void* buf, size_t n, off_t off)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t, off_t)> ("pwrite");
    return fn (fd, buf, n, off);
}

ssize_t pwrite64 (int fd, const void* buf, size_t n, off64_t off)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t, off64_t)> ("pwrite64");
    return fn (fd, buf, n, off);
}

ssize_t writev (int fd, const struct iovec* iov, int count)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, const struct iovec*, int)> ("writev");
    return fn (fd, iov, count);
}

ssize_t read (int fd, void* buf, size_t n)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, void*, size_t)> ("read");
    return fn (fd, buf, n);
}

int fsync (int fd)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int)> ("fsync");
    return fn (fd);
}

int fdatasync (int fd)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int)> ("fdatasync");
    return fn (fd);
}

int ftruncate (int fd, off_t length)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, off_t)> ("ftruncate");
    return fn (fd, length);
}

int ftruncate64 (int fd, off64_t length)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, off64_t)> ("ftruncate64");
    return fn (fd, length);
}

off_t lseek (int fd, off_t off, int whence)
{
    touchFd (fd);
    static const auto fn = real<off_t (*) (int, off_t, int)> ("lseek");
    return fn (fd, off, whence);
}

off64_t lseek64 (int fd, off64_t off, int whence)
{
    touchFd (fd);
    static const auto fn = real<off64_t (*) (int, off64_t, int)> ("lseek64");
    return fn (fd, off, whence);
}

int fstat (int fd, struct stat* st)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, struct stat*)> ("fstat");
    return fn (fd, st);
}

int fstat64 (int fd, struct stat64* st)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, struct stat64*)> ("fstat64");
    return fn (fd, st);
}

int stat (const char* path, struct stat* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat*)> ("stat");
    return fn (path, st);
}

int stat64 (const char* path, struct stat64* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat64*)> ("stat64");
    return fn (path, st);
}

int lstat (const char* path, struct stat* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat*)> ("lstat");
    return fn (path, st);
}

int lstat64 (const char* path, struct stat64* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat64*)> ("lstat64");
    return fn (path, st);
}

int fstatat (int dirfd, const char* path, struct stat* st, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, struct stat*, int)> ("fstatat");
    return fn (dirfd, path, st, flags);
}

int fstatat64 (int dirfd, const char* path, struct stat64* st, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, struct stat64*, int)> ("fstatat64");
    return fn (dirfd, path, st, flags);
}

int statx (int dirfd, const char* path, int flags, unsigned mask, struct statx* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, unsigned, struct statx*)> ("statx");
    return fn (dirfd, path, flags, mask, out);
}

int access (const char* path, int mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int)> ("access");
    return fn (path, mode);
}

int faccessat (int dirfd, const char* path, int mode, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, int)> ("faccessat");
    return fn (dirfd, path, mode, flags);
}

int mkdir (const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, mode_t)> ("mkdir");
    return fn (path, mode);
}

int mkdirat (int dirfd, const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, mode_t)> ("mkdirat");
    return fn (dirfd, path, mode);
}

DIR* opendir (const char* path)
{
    touchPath (path);
    static const auto fn = real<DIR* (*) (const char*)> ("opendir");
    return fn (path);
}

int rename (const char* from, const char* to)
{
    touchPath (from);
    touchPath (to);
    static const auto fn = real<int (*) (const char*, const char*)> ("rename");
    return fn (from, to);
}

int unlink (const char* path)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*)> ("unlink");
    return fn (path);
}

int rmdir (const char* path)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*)> ("rmdir");
    return fn (path);
}

int statvfs (const char* path, struct statvfs* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statvfs*)> ("statvfs");
    return fn (path, out);
}

int statvfs64 (const char* path, struct statvfs64* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statvfs64*)> ("statvfs64");
    return fn (path, out);
}

int statfs (const char* path, struct statfs* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statfs*)> ("statfs");
    return fn (path, out);
}

int statfs64 (const char* path, struct statfs64* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statfs64*)> ("statfs64");
    return fn (path, out);
}

char* realpath (const char* path, char* resolved)
{
    touchPath (path);
    static const auto fn = real<char* (*) (const char*, char*)> ("realpath");
    return fn (path, resolved);
}

} // extern "C"
