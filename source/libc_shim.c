/* Bionic-to-newlib compatibility wrappers for the Android Unity modules.
 * Socket, file, memory and synchronization ABIs are translated here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>

#include "config.h"
#include "error.h"
#include "imports.h"
#include "so_util.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "asset_pack.h"

/* Fortify wrappers ignore the object-size argument. */
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

/* Android system properties queried by Unity. */
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

static int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

/* Hashed futex wait queues for IL2CPP synchronization. */
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_BUCKETS     256

static Mutex   futex_lock[FUTEX_BUCKETS];   // libnx Mutex/CondVar are u32; 0 == ready
static CondVar futex_cond[FUTEX_BUCKETS];

static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    long ret = 0;
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      errno = EAGAIN; ret = -1;
    } else if (to) {
      const u64 ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns))) {
        errno = ETIMEDOUT; ret = -1;
      }
    } else {
      condvarWaitTimeout(&futex_cond[h], &futex_lock[h], 16000000ULL); // capped infinite wait
    }
    mutexUnlock(&futex_lock[h]);
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    mutexLock(&futex_lock[h]);
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  errno = ENOSYS;
  return -1;
}

struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate process_vm_* source ranges. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Support validated copies within the current process. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *source = writing ? lp : rp;
        if (!nx_addr_readable((uintptr_t)source, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
void android_set_abort_message_fake(const char *msg) { (void)msg; }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    /* Keep Unity's initial reservations within the wrapper arenas. */
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

/* Add the devoptab prefix omitted from managed paths. */
static const char *dev_abs(const char *in, char *buf, size_t n) {
  if (!in || in[0] != '/') return in;
  snprintf(buf, n, "sdmc:%s", in);
  return buf;
}
/* Retry Android asset paths against the staged assets tree. */
static int assets_suffix_fallback(const char *path, char *out, size_t outsz) {
  const char *hit = NULL, *s;
  for (s = path; (s = strstr(s, "assets/")) != NULL; s++) {
    if (s == path || s[-1] == '/' || s[-1] == '!') hit = s;
  }
  if (!hit) return 0;
  struct stat st;
  snprintf(out, outsz, "%s", hit);
  return stat(out, &st) == 0;
}

/* Skip devoptab root paths that fsdev cannot create. */
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

/* Create parents below the game root. */
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[640];   // >= dev_abs's 600B normalized path
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const size_t glen = strlen(GAME_HOME);
  if (strncmp(tmp, GAME_HOME, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
static void mkdir_parents(const char *filepath) {
  char tmp[640];   // >= dev_abs's 600B normalized path
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  return r;
}

int remove_fake(const char *path) {
  if (!path) { errno = EINVAL; return -1; }
  char nb[600];
  return remove(dev_abs(path, nb, sizeof nb));
}

int rename_fake(const char *old_path, const char *new_path) {
  if (!old_path || !new_path) { errno = EINVAL; return -1; }
  char old_nb[600], new_nb[600];
  old_path = dev_abs(old_path, old_nb, sizeof old_nb);
  new_path = dev_abs(new_path, new_nb, sizeof new_nb);
  mkdir_parents(new_path);
  return rename(old_path, new_path);
}

int rmdir_fake(const char *path) {
  if (!path) { errno = EINVAL; return -1; }
  char nb[600];
  return rmdir(dev_abs(path, nb, sizeof nb));
}

int unlink_fake(const char *path) {
  if (!path) { errno = EINVAL; return -1; }
  char nb[600];
  return unlink(dev_abs(path, nb, sizeof nb));
}

int truncate_fake(const char *path, long length) {
  if (!path || length < 0) { errno = EINVAL; return -1; }
  char nb[600];
  return truncate(dev_abs(path, nb, sizeof nb), (off_t)length);
}

int ftruncate_fake(int fd, long length) {
  if (length < 0) { errno = EINVAL; return -1; }
  if (asset_pack_fd_is(fd)) { errno = EROFS; return -1; }
  return ftruncate(fd, (off_t)length);
}

int fsync_fake(int fd) {
  if (asset_pack_fd_is(fd)) return 0;
  return fsync(fd);
}

/* Read-ahead windows for Unity archives. */
#define RA_SLOTS 8
#define RA_WIN   (1u << 20)     /* 1 MB read-ahead window */
static struct RaCache {
  int  fd;           /* -1 == free */
  long pos;          /* virtual file position (what read/lseek observe) */
  long size;         /* file size (for SEEK_END) */
  long base;         /* file offset of buf[0] */
  long len;          /* valid bytes currently in buf */
  unsigned char *buf;
} g_ra[RA_SLOTS] = {
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
  { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
};
static Mutex g_ra_lock;
static struct RaCache *ra_find(int fd) {
  if (fd < 0) return NULL;
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd == fd) return &g_ra[i];
  return NULL;
}
void ra_attach(int fd, long size) {
  mutexLock(&g_ra_lock);
  for (int i = 0; i < RA_SLOTS; i++) if (g_ra[i].fd < 0) {
    if (!g_ra[i].buf) g_ra[i].buf = malloc(RA_WIN);
    if (g_ra[i].buf) { g_ra[i].fd = fd; g_ra[i].pos = 0; g_ra[i].size = size; g_ra[i].base = 0; g_ra[i].len = 0; }
    break;
  }
  mutexUnlock(&g_ra_lock);
}
static void ra_detach(int fd) {
  mutexLock(&g_ra_lock);
  struct RaCache *c = ra_find(fd);
  if (c) c->fd = -1;   /* keep buf allocated for reuse */
  mutexUnlock(&g_ra_lock);
}
static long ra_read(struct RaCache *c, int fd, void *buf, size_t count) {
  size_t done = 0;
  mutexLock(&g_ra_lock);
  while (done < count) {
    if (c->len == 0 || c->pos < c->base || c->pos >= c->base + c->len) {
      if (lseek(fd, c->pos, SEEK_SET) < 0) break;
      long r = 0;
      while (r < (long)RA_WIN) { long k = read(fd, c->buf + r, RA_WIN - r); if (k <= 0) break; r += k; }
      if (r <= 0) break;
      c->base = c->pos; c->len = r;
    }
    long avail = (c->base + c->len) - c->pos;
    if (avail <= 0) break;
    size_t n = (count - done < (size_t)avail) ? count - done : (size_t)avail;
    memcpy((char *)buf + done, c->buf + (c->pos - c->base), n);
    c->pos += n; done += n;
  }
  mutexUnlock(&g_ra_lock);
  return (long)done;
}

/* off_t is 64-bit on this target, so this also services lseek64. */
long z_lseek(int fd, long off, int whence) {
  if (asset_pack_fd_is(fd)) return asset_pack_lseek_fd(fd, off, whence);
  struct RaCache *c = ra_find(fd);
  if (c) {   /* virtualized position -- don't touch the real fd here */
    mutexLock(&g_ra_lock);
    long np = (whence == SEEK_SET) ? off : (whence == SEEK_CUR) ? c->pos + off : c->size + off;
    c->pos = np;
    mutexUnlock(&g_ra_lock);
    return np;
  }
  return lseek(fd, off, whence);
}

static const char *synthetic_proc(const char *path);  /* defined below */

/* Materialize synthetic procfs data behind a real descriptor. */
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", GAME_HOME, safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

/* Stable synthetic inode numbers for files reported as inode zero by fsdev. */
#define FD_INO_MAX 4096
static uint64_t g_fd_ino[FD_INO_MAX];
static uint64_t path_ino(const char *path) {
  uint64_t h = 1469598103934665603ULL;               // FNV-1a 64 offset basis
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  return h ? h : 1;                                   // 0 means "no inode" -- avoid it
}
static void fd_ino_set(int fd, const char *path) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = path_ino(path); }
static void fd_ino_clear(int fd) { if (fd >= 0 && fd < FD_INO_MAX) g_fd_ino[fd] = 0; }

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
    /* Back Android random devices with libnx entropy. */
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth_dev_random", GAME_HOME);
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      fd_ino_set(rfd, path);
      return rfd;
    }
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { fd_ino_set(sfd, path); return sfd; }
    int packed_fd = asset_pack_open_path(path);
    if (packed_fd >= 0) {
      fd_ino_set(packed_fd, path);
      return packed_fd;
    }
  }
  /* Preserve literal virtual paths before adding the devoptab prefix. */
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  int fd = open(path, cvt, mode);
  if (fd < 0 && writing) {
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[512];
    if (assets_suffix_fallback(path, alt, sizeof(alt))) {
      fd = open(alt, cvt, mode);
    }
  }
  if (fd >= 0) {
    fd_ino_set(fd, path);
    struct stat _st;
    if (fstat(fd, &_st) == 0) {
      /* Cache large read-only assets. */
      if (!writing && _st.st_size >= (4 << 20))
        ra_attach(fd, (long)_st.st_size);
    }
  }
  return fd;
}
struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino;
  /* fsdev permissions do not describe SD write access. */
  out->st_mode = (in->st_mode & (uint32_t)S_IFMT) | 0777u;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_stat_path_info(path, &packed_size, &packed_ino, &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = (packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  struct stat real; int r = stat(path, &real);
  if (r != 0) {
    char alt[512];
    if (assets_suffix_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) st->st_ino = path_ino(path);   // fsdev gives 0 -> synth
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  uint64_t packed_size, packed_ino;
  int packed_directory;
  if (asset_pack_fstat_fd(fd, &packed_size, &packed_ino, &packed_directory)) {
    memset(st, 0, sizeof(*st));
    st->st_ino = packed_ino;
    st->st_mode = (packed_directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (int64_t)packed_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((packed_size + 511) / 512);
    return 0;
  }
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) {
    convert_stat(&real, st);
    if (st->st_ino == 0) {                               // mirror stat(path)'s inode
      uint64_t ino = (fd >= 0 && fd < FD_INO_MAX) ? g_fd_ino[fd] : 0;
      st->st_ino = ino ? ino : ((uint64_t)(fd + 1) * 2654435761ULL) | 1;
    }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  memset(&out, 0, sizeof(out));
  out.d_reclen = sizeof(out);
  if (asset_pack_dir_is(dirp)) {
    const char *name = asset_pack_readdir_path(dirp, &out.d_type, &out.d_ino);
    if (!name) return NULL;
    snprintf(out.d_name, sizeof(out.d_name), "%s", name);
  } else {
    struct dirent *e = readdir((DIR *)dirp);
    if (!e) return NULL;
    out.d_ino = e->d_ino;
    out.d_type = e->d_type;
    snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  }
  return &out;
}

int closedir_fake(void *dirp) {
  return asset_pack_dir_is(dirp) ? asset_pack_closedir_path(dirp)
                                 : closedir((DIR *)dirp);
}

/* Locale handles use newlib's C locale. */
void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

/* Page-granular mmap emulation for Unity's aligned reservations. */
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;

#define BIONIC_MAP_ANONYMOUS 0x20
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

/* Sparse reservations backed from a dedicated commit pool. */
static uint8_t *oc_base;        // stack-region window base (256MB-aligned)
static size_t   oc_pages;       // window size in pages (0 => OC disabled)
static uint8_t *oc_used;        // 1/page: address space reserved by an mmap
static uint8_t *oc_committed;   // 1/page: physically backed via svcMapMemory
static uint32_t *oc_phys;       // virtual page -> commit-pool page
static uint8_t *oc_pool;        // commit-pool base (heap, page-aligned)
static size_t   oc_pool_pages;  // pool capacity in pages
static uint8_t *oc_pool_used;   // reusable commit-pool bitmap

int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  size_t wp = window_bytes / MMAP_PAGE;
  uint8_t *u = (uint8_t *)calloc(wp, 1);
  uint8_t *c = (uint8_t *)calloc(wp, 1);
  uint32_t *p = (uint32_t *)malloc(wp * sizeof(*p));
  size_t pp = pool_bytes / MMAP_PAGE;
  uint8_t *pu = (uint8_t *)calloc(pp, 1);
  if (!u || !c || !p || !pu) { free(u); free(c); free(p); free(pu); return 0; }
  for (size_t i = 0; i < wp; i++) p[i] = UINT32_MAX;
  mutexLock(&g_mmap_lock);
  oc_base = (uint8_t *)window; oc_pages = wp; oc_used = u; oc_committed = c; oc_phys = p;
  oc_pool = (uint8_t *)pool; oc_pool_pages = pp; oc_pool_used = pu;
  mutexUnlock(&g_mmap_lock);
  return 1;
}

static int oc_contains(void *addr) {
  return oc_pages && (uint8_t *)addr >= oc_base &&
         (uint8_t *)addr < oc_base + oc_pages * MMAP_PAGE;
}

void oc_exclude_range(void *base, size_t len) {
  if (!oc_pages || (uint8_t *)base < oc_base) return;
  size_t first = ((uint8_t *)base - oc_base) / MMAP_PAGE;
  if (first >= oc_pages) return;
  size_t cnt = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  mutexLock(&g_mmap_lock);
  for (size_t i = 0; i < cnt; i++) oc_used[first + i] = 1;
  mutexUnlock(&g_mmap_lock);
}

static int oc_range_occupied(size_t i, size_t need) {
  uint64_t a   = (uint64_t)(uintptr_t)(oc_base + i * MMAP_PAGE);
  uint64_t end = a + (uint64_t)need * MMAP_PAGE;
  int occ = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) { occ = 1; break; }
    uint64_t span_end = mi.addr + mi.size;
    if (span_end <= a) { occ = 1; break; }
    if (mi.type != MemType_Unmapped) {
      occ = 1;
      uint64_t s = mi.addr > (uint64_t)(uintptr_t)oc_base ? mi.addr
                                                          : (uint64_t)(uintptr_t)oc_base;
      size_t p0 = (size_t)((s - (uint64_t)(uintptr_t)oc_base) / MMAP_PAGE);
      size_t p1 = (size_t)((span_end - (uint64_t)(uintptr_t)oc_base + MMAP_PAGE - 1) / MMAP_PAGE);
      for (size_t k = p0; k < p1 && k < oc_pages; k++) oc_used[k] = 1;
    }
    a = span_end;
  }
  return occ;
}

/* Reserve a 256 MB-aligned sparse range. */
static void *oc_alloc_locked(size_t len) {
  if (!oc_pages) return NULL;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE; if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  size_t kept = need > step ? need - step : need;
  for (size_t i = 0; i + need <= oc_pages; i += step) {        // pass 1: full over-map fits
    size_t run = 0; while (run < need && !oc_used[i + run]) run++;
    if (run == need) {
      if (oc_range_occupied(i, need)) continue;   // a thread stack landed here -> skip
      for (size_t k = 0; k < need; k++) oc_used[i + k] = 1;
      return oc_base + i * MMAP_PAGE;
    }
  }
  for (size_t i = 0; i < oc_pages; i += step) {                // pass 2: tail slot
    if (i + need <= oc_pages) continue;
    size_t avail = oc_pages - i; if (avail < kept) continue;
    size_t run = 0; while (run < avail && !oc_used[i + run]) run++;
    if (run == avail) {
      if (oc_range_occupied(i, avail)) continue;  // a thread stack landed here -> skip
      for (size_t k = 0; k < avail; k++) oc_used[i + k] = 1;
      return oc_base + i * MMAP_PAGE;
    }
  }
  return NULL;
}

static size_t oc_pool_run(size_t need, size_t *length) {
  for (size_t i = 0; i < oc_pool_pages; ) {
    while (i < oc_pool_pages && oc_pool_used[i]) i++;
    size_t start = i;
    while (i < oc_pool_pages && !oc_pool_used[i] && i - start < need) i++;
    if (i > start) { *length = i - start; return start; }
  }
  *length = 0;
  return 0;
}

static size_t oc_map_run(void *dst, void *src, size_t pages) {
  Result rc = 0;
  size_t attempt = pages;
  while (attempt) {
    rc = svcMapMemory(dst, src, (u64)attempt * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) return attempt;
    if (attempt == 1) {
      MemoryInfo dm = {0}, sm = {0};
      u32 dp = 0, sp = 0;
      svcQueryMemory(&dm, &dp, (u64)(uintptr_t)dst);
      svcQueryMemory(&sm, &sp, (u64)(uintptr_t)src);
      fatal_error("Could not commit mapped memory (0x%08x).\n"
                  "dst type=%x attr=%x perm=%x\n"
                  "src type=%x attr=%x perm=%x",
                  rc, dm.type, dm.attr, dm.perm, sm.type, sm.attr, sm.perm);
    }
    attempt = (attempt + 1) / 2;
  }
  return 0;
}

static void oc_commit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (oc_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !oc_committed[first + i + run]) run++;
    while (run) {
      size_t mapped = 0;
      size_t pool_page = oc_pool_run(run, &mapped);
      if (!mapped) fatal_error("Mapped-memory pool exhausted.");
      void *dst = oc_base + (first + i) * MMAP_PAGE;
      void *src = oc_pool + pool_page * MMAP_PAGE;
      mapped = oc_map_run(dst, src, mapped);
      memset(dst, 0, mapped * MMAP_PAGE);
      for (size_t k = 0; k < mapped; k++) {
        oc_committed[first + i + k] = 1;
        oc_phys[first + i + k] = (uint32_t)(pool_page + k);
        oc_pool_used[pool_page + k] = 1;
      }
      i += mapped;
      run -= mapped;
    }
  }
}

static void oc_decommit_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  for (size_t i = 0; i < cnt; ) {
    if (!oc_committed[first + i]) { i++; continue; }
    uint32_t pool_page = oc_phys[first + i];
    size_t run = 1;
    while (i + run < cnt && oc_committed[first + i + run] &&
           oc_phys[first + i + run] == pool_page + run) run++;
    void *dst = oc_base + (first + i) * MMAP_PAGE;
    void *src = oc_pool + (size_t)pool_page * MMAP_PAGE;
    if (R_FAILED(svcUnmapMemory(dst, src, (u64)run * MMAP_PAGE)))
      fatal_error("Could not release mapped memory.");
    memset(src, 0, run * MMAP_PAGE);
    for (size_t k = 0; k < run; k++) {
      oc_committed[first + i + k] = 0;
      oc_phys[first + i + k] = UINT32_MAX;
      oc_pool_used[pool_page + k] = 0;
    }
    i += run;
  }
}

static void oc_free_locked(void *addr, size_t len) {
  if ((uint8_t *)addr < oc_base) return;
  size_t first = ((uint8_t *)addr - oc_base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first >= oc_pages) return;
  if (first + cnt > oc_pages) cnt = oc_pages - first;
  oc_decommit_locked(addr, len);
  memset(oc_used + first, 0, cnt);
}

static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  if (!g_mmap_arena_base || !g_mmap_arena_size)
    fatal_error("Mapped-memory arena is unavailable. Use title override.");
  uint8_t *base = (uint8_t *)g_mmap_arena_base;
  size_t usable = g_mmap_arena_size;
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;
}

/* The final aligned slot may reserve only its in-arena prefix. */
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        *got = avail * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  mutexLock(&g_mmap_lock);
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++)
    mmap_used[first + k] = 0;
  mutexUnlock(&g_mmap_lock);
}

/* Heap-backed mappings tracked for munmap. */
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  /* Preserve Unity's alignment requirement for large anonymous pools. */
  size_t align = (length >= MMAP_BIG_THRESH && (flags & BIONIC_MAP_ANONYMOUS))
                   ? MMAP_BIG_ALIGN : MMAP_PAGE;
  void *q = memalign(align, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, q, length, offset);
        if (got < 0) got = 0;
      } else {
        long cur = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) >= 0)
          while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
        if (cur >= 0) lseek(fd, cur, SEEK_SET);
      }
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n < MMAP_FALLBACK_MAX) { g_fb[g_fb_n].ptr = q; g_fb[g_fb_n].len = length; g_fb_n++; }
  mutexUnlock(&g_fb_lock);
  return q;
}

static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      free(addr);
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

/* Read-only map deduplication. */
#define MAPC_N 24
static struct { uint64_t ino; long off; size_t len; void *ptr; } g_mapc[MAPC_N];
static int g_mapc_n = 0;
static void *mapcache_get(uint64_t ino, long off, size_t len) {
  void *r = NULL;
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_mapc_n; i++)
    if (g_mapc[i].ino == ino && g_mapc[i].off == off && g_mapc[i].len == len) { r = g_mapc[i].ptr; break; }
  mutexUnlock(&g_fb_lock);
  return r;
}
static void mapcache_put(uint64_t ino, long off, size_t len, void *ptr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++)          // pin: drop from fallback free-list
    if (g_fb[i].ptr == ptr) { g_fb[i] = g_fb[--g_fb_n]; break; }
  if (g_mapc_n < MAPC_N) { g_mapc[g_mapc_n].ino = ino; g_mapc[g_mapc_n].off = off;
                           g_mapc[g_mapc_n].len = len; g_mapc[g_mapc_n].ptr = ptr; g_mapc_n++; }
  mutexUnlock(&g_fb_lock);
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)addr;
  if (length == 0) length = 1;

  /* Commit large anonymous reservations lazily. */
  if (oc_pages && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length);
    mutexUnlock(&g_mmap_lock);
    if (op) return op;
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  /* File mappings require a complete contiguous reservation. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    mmap_arena_free(p, length);
    p = NULL;
  }
  if (!p) {
    /* Spill exhausted arena mappings into the heap. */
    int ro_file = fd >= 0 && !(flags & BIONIC_MAP_ANONYMOUS) && !(prot & BIONIC_PROT_WRITE);
    uint64_t mino = 0;
    if (ro_file) {
      uint64_t packed_size;
      if (!asset_pack_fstat_fd(fd, &packed_size, &mino, NULL) && fd < FD_INO_MAX)
        mino = g_fd_ino[fd];
    }
    if (mino) {
      void *hit = mapcache_get(mino, offset, length);
      if (hit) return hit;
    }
    void *q = mmap_fallback(length, flags, fd, offset);
    if (q) {
      if (mino) mapcache_put(mino, offset, length, q);
      return q;
    }
    errno = ENOMEM;
    return (void *)-1;
  }

  size_t fill = length < reserved ? length : reserved;

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    long got = 0;
    if (fd >= 0) {
      if (asset_pack_fd_is(fd)) {
        got = asset_pack_pread_fd(fd, p, fill, offset);
        if (got < 0) got = 0;
      } else {
        long cur = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) >= 0) {
          while ((size_t)got < fill) {
            long r = read(fd, (char *)p + got, fill - (size_t)got);
            if (r <= 0) break;
            got += r;
          }
        }
        if (cur >= 0) lseek(fd, cur, SEEK_SET);
      }
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  if (mmap_fallback_free(addr)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

int mprotect_fake(void *addr, size_t len, int prot) {
  if (oc_contains(addr)) {
    if (prot != BIONIC_PROT_NONE) {
      mutexLock(&g_mmap_lock);
      oc_commit_locked(addr, len);
      mutexUnlock(&g_mmap_lock);
    }
  }
  return 0;
}
int madvise_fake(void *addr, size_t len, int advice) {
  if (oc_contains(addr) && (advice == 4 || advice == 8)) {
    mutexLock(&g_mmap_lock);
    oc_decommit_locked(addr, len);
    mutexUnlock(&g_mmap_lock);
  }
  return 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
int statfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x78); return 0; }

/* Synthetic memory and CPU information used by Unity sizing logic. */
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

#define PACK_FILE_SLOTS 64
static struct { FILE *file; void *data; int fd; } g_pack_files[PACK_FILE_SLOTS];
static Mutex g_pack_file_lock;

static int packed_file_add(FILE *file, void *data, int fd) {
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (!g_pack_files[i].file) {
      g_pack_files[i].file = file;
      g_pack_files[i].data = data;
      g_pack_files[i].fd = fd;
      mutexUnlock(&g_pack_file_lock);
      return 1;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  return 0;
}

static FILE *packed_fopen(const char *path) {
  void *data = NULL;
  size_t size = 0;
  if (!asset_pack_read_all_path(path, &data, &size)) return NULL;
  FILE *file = fmemopen(data, size ? size : 1, "r");
  int fd = file ? asset_pack_open_path(path) : -1;
  if (!file || !packed_file_add(file, data, fd)) {
    if (fd >= 0) asset_pack_close_fd(fd);
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

FILE *fdopen_fake(int fd, const char *mode) {
  if (!asset_pack_fd_is(fd)) return fdopen(fd, mode);
  if (!mode || strpbrk(mode, "wa+")) { errno = EINVAL; return NULL; }
  uint64_t size;
  long position = asset_pack_lseek_fd(fd, 0, SEEK_CUR);
  if (!asset_pack_fstat_fd(fd, &size, NULL, NULL) || size > SIZE_MAX || position < 0) return NULL;
  void *data = malloc(size ? (size_t)size : 1);
  if (!data || (size && asset_pack_pread_fd(fd, data, (size_t)size, 0) != (long)size)) {
    free(data);
    return NULL;
  }
  FILE *file = fmemopen(data, size ? (size_t)size : 1, "r");
  if (!file || fseek(file, position, SEEK_SET) != 0 || !packed_file_add(file, data, fd)) {
    if (file) fclose(file);
    free(data);
    errno = EMFILE;
    return NULL;
  }
  return file;
}

static int packed_fileno(FILE *file) {
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++)
    if (g_pack_files[i].file == file) { fd = g_pack_files[i].fd; break; }
  mutexUnlock(&g_pack_file_lock);
  return fd;
}

static int packed_fclose(FILE *file) {
  void *data = NULL;
  int fd = -1;
  mutexLock(&g_pack_file_lock);
  for (int i = 0; i < PACK_FILE_SLOTS; i++) {
    if (g_pack_files[i].file == file) {
      data = g_pack_files[i].data;
      fd = g_pack_files[i].fd;
      g_pack_files[i].file = NULL;
      g_pack_files[i].data = NULL;
      g_pack_files[i].fd = -1;
      break;
    }
  }
  mutexUnlock(&g_pack_file_lock);
  if (!data) return 0;
  int result = fclose(file);
  if (fd >= 0) asset_pack_close_fd(fd);
  free(data);
  return result == 0 ? 1 : -1;
}

FILE *fopen_fake(const char *path, const char *mode) {
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    void *data = strdup(synth);
    FILE *file = data ? fmemopen(data, n ? n : 1, "r") : NULL;
    if (!file || !packed_file_add(file, data, -1)) {
      if (file) fclose(file);
      free(data);
      return NULL;
    }
    return file;
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!writing && strchr(mode, 'r')) {
    FILE *packed = packed_fopen(path);
    if (packed) return packed;
  }
  char _nb[600]; path = dev_abs(path, _nb, sizeof _nb);
  FILE *f = fopen(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = fopen(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[512];
    if (assets_suffix_fallback(path, alt, sizeof(alt))) {
      f = fopen(alt, mode);
    }
  }
  if (!f)
    return NULL;
  return f;
}

/* Silent bionic standard streams; real FILE handles pass through. */
uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) return 0; return fputs(s, f); }
int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (is_fake_file(f)) return 0;
  int packed = packed_fclose(f);
  if (packed) return packed < 0 ? -1 : 0;
  return fclose(f);
}
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) {
  if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  int packed = packed_fileno(f);
  return packed >= 0 ? packed : fileno(f);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  return vfprintf(f, fmt, va);
}

/* Route synthetic pipes and real files. */
long read_fake(int fd, void *buf, size_t count) {
  if (asset_pack_fd_is(fd)) {
    return asset_pack_read_fd(fd, buf, count);
  }
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
  {
    struct RaCache *c = ra_find(fd);
    if (c) return ra_read(c, fd, buf, count);
  }
  /* Fill large IL2CPP reads across fsdev short reads. */
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) {
      if (total) break;
      return -1;
    }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  return (long)total;
}
long write_fake(int fd, const void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  return write(fd, buf, count);
}
int close_fake(int fd) {
  if (asset_pack_fd_is(fd)) {
    fd_ino_clear(fd);
    return asset_pack_close_fd(fd);
  }
  ra_detach(fd);
  fd_ino_clear(fd);
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  return close(fd);
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }

/* Bionic socket ABI to libnx BSD socket bridge. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
int g_net_on = 1;

static int n2b_errno(int e) {
  switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114;
    case EISCONN: return 106; case ENOTCONN: return 107; case ECONNREFUSED: return 111;
    case ECONNRESET: return 104; case ECONNABORTED: return 103; case ETIMEDOUT: return 110;
    case ENETUNREACH: return 101; case EHOSTUNREACH: return 113; case ENETDOWN: return 100;
    case EADDRINUSE: return 98; case EADDRNOTAVAIL: return 99; case EINTR: return 4;
    case EPIPE: return 32; case EBADF: return 9; case EINVAL: return 22; case EACCES: return 13;
    case EAFNOSUPPORT: return 97; case EMFILE: return 24; case ENFILE: return 23;
    case EMSGSIZE: return 90; case ENOBUFS: return 105; case ENOMEM: return 12;
    case ENOTSOCK: return 88; case EPROTONOSUPPORT: return 93; case EOPNOTSUPP: return 95;
    default: return e;
  }
}
#define NET_FAIL() do { errno = n2b_errno(errno); } while (0)
static int af_b2n(int f) { return f == 10 ? AF_INET6 : f; }
static int af_n2b(int f) { return f == AF_INET6 ? 10 : f; }
static unsigned sa_b2n(const void *bsa, unsigned blen, unsigned char out[128]) {
  if (!bsa || blen < 2) return 0;
  unsigned n = blen > 128 ? 128 : blen;
  memcpy(out, bsa, n);
  const unsigned char *b = (const unsigned char *)bsa;
  out[0] = (unsigned char)n;                       /* BSD sin_len */
  out[1] = (unsigned char)af_b2n(b[0] | (b[1] << 8)); /* BSD u8 family */
  return n;
}
static void sa_n2b(const unsigned char *nsa, unsigned n, void *bout, unsigned *boutlen) {
  if (!bout) { if (boutlen) *boutlen = 0; return; }
  unsigned c = n > 128 ? 128 : n;
  if (c >= 2) { memcpy(bout, nsa, c); int fam = af_n2b(nsa[1]);
                ((unsigned char *)bout)[0] = (unsigned char)(fam & 0xFF);
                ((unsigned char *)bout)[1] = (unsigned char)((fam >> 8) & 0xFF); }
  if (boutlen) *boutlen = c;
}
#define B_SOCK_NONBLOCK 0x800
#define B_SOCK_CLOEXEC  0x80000
#define B_MSG_NOSIGNAL  0x4000
#define B_MSG_DONTWAIT  0x40
static int msg_b2n(int f) {
  int o = f & ~(B_MSG_NOSIGNAL | B_MSG_DONTWAIT);
  if (f & B_MSG_NOSIGNAL) o |= MSG_NOSIGNAL;
  if (f & B_MSG_DONTWAIT) o |= MSG_DONTWAIT;
  return o;
}
static int opt_b2n(int *level, int *name) {   /* 0 ok, -1 skip */
  if (*level == 1) { *level = SOL_SOCKET;
    switch (*name) {
      case 2:  *name = SO_REUSEADDR; return 0; case 3:  *name = SO_TYPE;      return 0;
      case 4:  *name = SO_ERROR;     return 0; case 6:  *name = SO_BROADCAST; return 0;
      case 7:  *name = SO_SNDBUF;    return 0; case 8:  *name = SO_RCVBUF;    return 0;
      case 9:  *name = SO_KEEPALIVE; return 0; case 13: *name = SO_LINGER;    return 0;
      case 20: *name = SO_RCVTIMEO;  return 0; case 21: *name = SO_SNDTIMEO;  return 0;
      default: return -1;
    }
  }
  return 0;   /* IPPROTO_TCP(6)/IP(0): TCP_NODELAY=1 shared -> pass through */
}

int poll_fake(void *fds, unsigned long nfds, int timeout) {
  int r = poll((struct pollfd *)fds, (nfds_t)nfds, timeout); if (r < 0) NET_FAIL(); return r;
}
int select_fake(int n, void *r, void *w, void *e, void *t) {
  int rc = select(n, (fd_set *)r, (fd_set *)w, (fd_set *)e, (struct timeval *)t);
  if (rc < 0) NET_FAIL();
  return rc;
}
int socket_fake(int d, int t, int p) {
  if (!g_net_on) { errno = 97 /*EAFNOSUPPORT*/; return -1; }
  int nb = (t & B_SOCK_NONBLOCK) != 0;
  int fd = socket(af_b2n(d), t & ~(B_SOCK_NONBLOCK | B_SOCK_CLOEXEC), p);
  if (fd < 0) { NET_FAIL(); return -1; }
  if (nb) { int fl = fcntl(fd, F_GETFL, 0); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
  return fd;
}
int connect_fake(int s, const void *a, unsigned l) {
  unsigned char nsa[128]; unsigned n = sa_b2n(a, l, nsa);
  int r = connect(s, (const struct sockaddr *)nsa, n);
  if (r < 0) NET_FAIL();
  return r;
}
long recvmsg_fake(int s, void *msg, int flags) { long r = recvmsg(s, (struct msghdr *)msg, msg_b2n(flags)); if (r < 0) NET_FAIL(); return r; }
long sendmsg_fake(int s, const void *msg, int flags) { long r = sendmsg(s, (const struct msghdr *)msg, msg_b2n(flags)); if (r < 0) NET_FAIL(); return r; }
int inet_pton_shim(int af, const char *src, void *dst) { return inet_pton(af_b2n(af), src, dst); }
const char *inet_ntop_shim(int af, const void *src, char *dst, unsigned size) { return inet_ntop(af_b2n(af), src, dst, size); }
int bind_fake(int s, const void *a, unsigned l) {
  unsigned char nsa[128]; unsigned n = sa_b2n(a, l, nsa);
  int r = bind(s, (const struct sockaddr *)nsa, n); if (r < 0) NET_FAIL(); return r;
}
int listen_fake(int s, int b) { int r = listen(s, b); if (r < 0) NET_FAIL(); return r; }
int accept_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = accept(s, a ? (struct sockaddr *)nsa : NULL, a ? &nl : NULL);
  if (r < 0) { NET_FAIL(); return -1; }
  if (a && l) sa_n2b(nsa, nl, a, (unsigned *)l);
  return r;
}
long send_fake(int s, const void *b, size_t l, int f) { long r = send(s, b, l, msg_b2n(f)); if (r < 0) NET_FAIL(); return r; }
long recv_fake(int s, void *b, size_t l, int f) { long r = recv(s, b, l, msg_b2n(f)); if (r < 0) NET_FAIL(); return r; }
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  long r = recvfrom(s, b, l, msg_b2n(f), a ? (struct sockaddr *)nsa : NULL, a ? &nl : NULL);
  if (r < 0) { NET_FAIL(); return -1; }
  if (a && al) sa_n2b(nsa, nl, a, (unsigned *)al);
  return r;
}
int shutdown_fake(int s, int how) { int r = shutdown(s, how); if (r < 0) NET_FAIL(); return r; }
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) {
  int L = lv, N = n; if (opt_b2n(&L, &N) < 0) return 0;
  int r = setsockopt(s, L, N, v, l); if (r < 0) NET_FAIL(); return r;
}
int getsockopt_fake(int s, int lv, int n, void *v, void *l) {
  int L = lv, N = n; if (opt_b2n(&L, &N) < 0) { if (l) *(unsigned *)l = 0; return 0; }
  int r = getsockopt(s, L, N, v, (unsigned *)l); if (r < 0) { NET_FAIL(); return -1; }
  if (L == SOL_SOCKET && N == SO_ERROR && v && l && *(unsigned *)l >= 4) *(int *)v = n2b_errno(*(int *)v);
  return r;
}
int getsockname_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = getsockname(s, (struct sockaddr *)nsa, &nl); if (r < 0) { NET_FAIL(); return -1; }
  sa_n2b(nsa, nl, a, (unsigned *)l); return 0;
}
int getpeername_fake(int s, void *a, void *l) {
  unsigned char nsa[128]; unsigned nl = sizeof nsa;
  int r = getpeername(s, (struct sockaddr *)nsa, &nl); if (r < 0) { NET_FAIL(); return -1; }
  sa_n2b(nsa, nl, a, (unsigned *)l); return 0;
}
struct b_addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; unsigned ai_addrlen;
                    void *ai_addr; char *ai_canonname; struct b_addrinfo *ai_next; };
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) {
  if (res) *res = NULL;
  if (!g_net_on) return -2 /*EAI_NONAME*/;
  struct addrinfo nh, *nhp = NULL; const struct b_addrinfo *bh = (const struct b_addrinfo *)hints;
  if (bh) { memset(&nh, 0, sizeof nh); nh.ai_flags = bh->ai_flags; nh.ai_family = af_b2n(bh->ai_family);
            nh.ai_socktype = bh->ai_socktype; nh.ai_protocol = bh->ai_protocol; nhp = &nh; }
  struct addrinfo *nres = NULL;
  int rc = getaddrinfo(node, svc, nhp, &nres);
  if (rc != 0) return rc;
  struct b_addrinfo *head = NULL, *tail = NULL;
  for (struct addrinfo *p = nres; p; p = p->ai_next) {
    struct b_addrinfo *b = (struct b_addrinfo *)calloc(1, sizeof *b); if (!b) break;
    b->ai_flags = p->ai_flags; b->ai_family = af_n2b(p->ai_family);
    b->ai_socktype = p->ai_socktype; b->ai_protocol = p->ai_protocol;
    if (p->ai_addr && p->ai_addrlen) { unsigned char *ba = (unsigned char *)calloc(1, p->ai_addrlen); unsigned bl = p->ai_addrlen;
      if (ba) { sa_n2b((const unsigned char *)p->ai_addr, p->ai_addrlen, ba, &bl); b->ai_addr = ba; b->ai_addrlen = bl; } }
    if (p->ai_canonname) b->ai_canonname = strdup(p->ai_canonname);
    if (!head) head = b; else tail->ai_next = b; tail = b;
  }
  freeaddrinfo(nres);
  if (res) *res = head;
  return head ? 0 : 2 /*EAI_AGAIN*/;
}
void freeaddrinfo_fake(void *res) {
  struct b_addrinfo *p = (struct b_addrinfo *)res;
  while (p) { struct b_addrinfo *n = p->ai_next; free(p->ai_addr); free(p->ai_canonname); free(p); p = n; }
}
/* Translate bionic O_NONBLOCK. */
int fcntl_shim(int fd, int cmd, int arg) {
  if (asset_pack_fd_is(fd)) {
    if (cmd == 0 /*F_DUPFD*/ || cmd == 1030 /*F_DUPFD_CLOEXEC*/)
      return asset_pack_dup_fd(fd);
    if (cmd == 3 /*F_GETFL*/) return O_RDONLY;
    if (cmd == 4 /*F_SETFL*/ || cmd == 2 /*F_SETFD*/ || cmd == 1 /*F_GETFD*/) return 0;
    errno = EINVAL;
    return -1;
  }
  if (cmd == 3 /*F_GETFL*/) {
    int fl = fcntl(fd, F_GETFL, 0); if (fl < 0) { NET_FAIL(); return -1; }
    return (fl & ~O_NONBLOCK) | ((fl & O_NONBLOCK) ? 0x800 : 0);
  }
  if (cmd == 4 /*F_SETFL*/) {
    int a = (arg & ~0x800) | ((arg & 0x800) ? O_NONBLOCK : 0);
    int r = fcntl(fd, F_SETFL, a); if (r < 0) NET_FAIL(); return r;
  }
  if (cmd == 2 /*F_SETFD*/ || cmd == 1 /*F_GETFD*/) return 0;   /* CLOEXEC no-op */
  int r = fcntl(fd, cmd, arg); if (r < 0) NET_FAIL(); return r;
}
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) { (void)a; (void)al; (void)f; if (h && hl) h[0] = 0; if (s && sl) s[0] = 0; return -1; }
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
/* Bionic passwd layout. */
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", dir[] = GAME_HOME, sh[] = "/bin/sh", empty[] = "";
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    if (!strcmp(name, "HOME"))   return (char *)managed_path(GAME_HOME);
    if (!strcmp(name, "TMPDIR")) return (char *)managed_path(GAME_HOME);
  }
  return getenv(name);
}
/* Managed paths use Unix roots without devoptab prefixes. */
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}

void *dlopen_fake(const char *name, int flags) { (void)name; (void)flags; return (void *)0x1; }
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }
void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  extern void *firebase_stub_lookup(const char *symbol);
  void *p = so_resolve_external(symbol);
  if (p) return p;
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) return (void *)shim;
  /* Firebase managed bindings use a minimal native dependency surface. */
  void *fb = firebase_stub_lookup(symbol);
  if (fb) return fb;
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) return p;
  }
  return NULL;
}

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
int pthread_rwlock_rdlock_fake(void **rw) { rwlockReadLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { rwlockWriteLock(&get_rwlock(rw)->lock); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct { Semaphore sem; } FakeSem;
int sem_init_fake(void **s, int pshared, unsigned int value) { (void)pshared; FakeSem *fs = calloc(1, sizeof(*fs)); semaphoreInit(&fs->sem, value); *s = fs; return 0; }
int sem_destroy_fake(void **s) { if (s && *s) { free(*s); *s = NULL; } return 0; }
int sem_post_fake(void **s) { if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem); return 0; }
int sem_wait_fake(void **s) { if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem); return 0; }
static int sem_trywait_fake(void **s) { if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0; errno = EAGAIN; return -1; }
int sem_getvalue_fake(void **s, int *val) { if (s && *s) *val = (int)((FakeSem *)*s)->sem.count; else *val = 0; return 0; }
/* libnx Semaphore has no timed wait. */
int sem_timedwait_fake(void **s, const struct timespec *abs) {
  (void)abs;
  for (int i = 0; i < 1000; i++) {
    if (sem_trywait_fake(s) == 0) return 0;
    svcSleepThread(1000000ull); // 1 ms
  }
  errno = ETIMEDOUT;
  return -1;
}

/* Acknowledge IL2CPP GC signals that libnx cannot deliver. */
uintptr_t g_il2cpp_base = 0;

/* IL2CPP 2022.3.62f2 GC signal and acknowledgement globals. */
#define GC_SUSPEND_SIG_OFF 0x4CE292C
#define GC_RESTART_SIG_OFF 0x4CE2930
#define GC_START_ACK_OFF   0x4CE2928
#define GC_ACK_SEM_OFF     0x4F0D298

int pthread_kill_gc(pthread_t t, int sig) {
  (void)t;
  uintptr_t b = g_il2cpp_base;
  if (b && sig) {
    int suspend_sig = *(volatile int *)(b + GC_SUSPEND_SIG_OFF);
    int restart_sig = *(volatile int *)(b + GC_RESTART_SIG_OFF);
    void **ack_sem  = (void **)(b + GC_ACK_SEM_OFF);
    if (sig == suspend_sig) {            /* stop-the-world: ack the suspend */
      sem_post_fake(ack_sem);
      return 0;
    }
    if (sig == restart_sig) {            /* start-the-world: ack iff handler would */
      if (*(volatile int *)(b + GC_START_ACK_OFF)) sem_post_fake(ack_sem);
      return 0;
    }
  }
  return 0;   /* any other signal: no-op, as before */
}
