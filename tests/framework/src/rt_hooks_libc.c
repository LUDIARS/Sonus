/*
 * libc interposition for RT-safety detection in test executables (design.md 14).
 *
 * Active only on glibc without sanitizers (sanitizer runtimes interpose the
 * same symbols). Each wrapper reports to snd::rt::Report (which does nothing
 * outside an RT scope) and forwards to the real implementation:
 *   - heap: __libc_malloc & co. (exported by glibc)
 *   - locks / file IO: next definition found with dlsym(RTLD_NEXT)
 * Coverage is best effort (libc-internal calls bypass the PLT); library code
 * additionally places explicit SND_RT_NOTE() hooks.
 */
#if defined(_FORTIFY_SOURCE)
#undef _FORTIFY_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stddef.h>

void snd_test_rt_report(int kind, const char* site);
void snd_test_rt_mark_hooks(int libc, int newDelete);
void snd_test_rt_suspend(int enter);

/* Must match snd::rt::Violation. */
enum { SND_V_ALLOC = 0, SND_V_FREE = 1, SND_V_LOCK = 2, SND_V_FILEIO = 3 };

#if defined(__GLIBC__) || (defined(__linux__) && !defined(__ANDROID__))
#include <features.h>
#endif

#if defined(__GLIBC__) && !defined(SND_SANITIZER_ACTIVE)

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

extern void* __libc_malloc(size_t);
extern void __libc_free(void*);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);

__attribute__((constructor)) static void snd_test_hooks_init(void) { snd_test_rt_mark_hooks(1, 0); }

void* malloc(size_t n) {
  snd_test_rt_report(SND_V_ALLOC, "malloc");
  return __libc_malloc(n);
}
void free(void* p) {
  if (p != NULL) snd_test_rt_report(SND_V_FREE, "free");
  __libc_free(p);
}
void* calloc(size_t n, size_t s) {
  snd_test_rt_report(SND_V_ALLOC, "calloc");
  return __libc_calloc(n, s);
}
void* realloc(void* p, size_t n) {
  snd_test_rt_report(SND_V_ALLOC, "realloc");
  return __libc_realloc(p, n);
}
void* aligned_alloc(size_t align, size_t n) {
  snd_test_rt_report(SND_V_ALLOC, "aligned_alloc");
  return __libc_memalign(align, n);
}
void* memalign(size_t align, size_t n) {
  snd_test_rt_report(SND_V_ALLOC, "memalign");
  return __libc_memalign(align, n);
}
int posix_memalign(void** out, size_t align, size_t n) {
  void* p;
  snd_test_rt_report(SND_V_ALLOC, "posix_memalign");
  if (align < sizeof(void*) || (align & (align - 1)) != 0) return 22; /* EINVAL */
  p = __libc_memalign(align, n);
  if (p == NULL) return 12; /* ENOMEM */
  *out = p;
  return 0;
}

/* --- symbols resolved with dlsym(RTLD_NEXT) ---------------------------------- */
static void* snd_next(const char* name) {
  void* sym;
  snd_test_rt_suspend(1); /* dlsym may allocate */
  sym = dlsym(RTLD_NEXT, name);
  snd_test_rt_suspend(0);
  return sym;
}

#define SND_RESOLVE(var, name)                \
  do {                                        \
    if ((var) == NULL) {                      \
      void* s_ = snd_next(name);              \
      memcpy(&(var), &s_, sizeof(s_));        \
    }                                         \
  } while (0)

typedef int (*snd_mutex_fn)(pthread_mutex_t*);
typedef int (*snd_rwlock_fn)(pthread_rwlock_t*);
static snd_mutex_fn snd_real_mutex_lock;
static snd_mutex_fn snd_real_mutex_trylock;
static snd_rwlock_fn snd_real_rdlock;
static snd_rwlock_fn snd_real_wrlock;

int pthread_mutex_lock(pthread_mutex_t* m) {
  snd_test_rt_report(SND_V_LOCK, "pthread_mutex_lock");
  SND_RESOLVE(snd_real_mutex_lock, "pthread_mutex_lock");
  return snd_real_mutex_lock(m);
}
int pthread_mutex_trylock(pthread_mutex_t* m) {
  snd_test_rt_report(SND_V_LOCK, "pthread_mutex_trylock");
  SND_RESOLVE(snd_real_mutex_trylock, "pthread_mutex_trylock");
  return snd_real_mutex_trylock(m);
}
int pthread_rwlock_rdlock(pthread_rwlock_t* l) {
  snd_test_rt_report(SND_V_LOCK, "pthread_rwlock_rdlock");
  SND_RESOLVE(snd_real_rdlock, "pthread_rwlock_rdlock");
  return snd_real_rdlock(l);
}
int pthread_rwlock_wrlock(pthread_rwlock_t* l) {
  snd_test_rt_report(SND_V_LOCK, "pthread_rwlock_wrlock");
  SND_RESOLVE(snd_real_wrlock, "pthread_rwlock_wrlock");
  return snd_real_wrlock(l);
}

typedef FILE* (*snd_fopen_fn)(const char*, const char*);
typedef int (*snd_open_fn)(const char*, int, ...);
typedef ssize_t (*snd_read_fn)(int, void*, size_t);
typedef ssize_t (*snd_write_fn)(int, const void*, size_t);
static snd_fopen_fn snd_real_fopen;
static snd_open_fn snd_real_open;
static snd_read_fn snd_real_read;
static snd_write_fn snd_real_write;

FILE* fopen(const char* path, const char* mode) {
  snd_test_rt_report(SND_V_FILEIO, "fopen");
  SND_RESOLVE(snd_real_fopen, "fopen");
  return snd_real_fopen(path, mode);
}
int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  snd_test_rt_report(SND_V_FILEIO, "open");
  SND_RESOLVE(snd_real_open, "open");
  if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, unsigned int);
    va_end(ap);
  }
  return snd_real_open(path, flags, mode);
}
ssize_t read(int fd, void* buf, size_t n) {
  snd_test_rt_report(SND_V_FILEIO, "read");
  SND_RESOLVE(snd_real_read, "read");
  return snd_real_read(fd, buf, n);
}
ssize_t write(int fd, const void* buf, size_t n) {
  snd_test_rt_report(SND_V_FILEIO, "write");
  SND_RESOLVE(snd_real_write, "write");
  return snd_real_write(fd, buf, n);
}

#else

/* Sanitizer or non-glibc build: only operator new/delete and SND_RT_NOTE hooks. */
int snd_test_rt_hooks_libc_unavailable = 1;

#endif
