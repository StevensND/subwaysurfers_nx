/* Bionic-to-libnx libc compatibility. */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>

/* Strip a devoptab device prefix from paths returned to managed code. */
const char *managed_path(const char *p);

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
void __FD_SET_chk_fake(int fd, void *set, size_t setlen);
int  __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen);

int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
long syscall_fake(long number, ...);
void sincosf_fake(float x, float *s, float *c);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
long sysconf_fake(int name);

// fs
int open_fake(const char *path, int flags, ...);
int mkdir_fake(const char *path, unsigned mode);
struct bionic_stat;
int stat_fake(const char *path, struct bionic_stat *st);
int fstat_fake(int fd, struct bionic_stat *st);
int lstat_fake(const char *path, struct bionic_stat *st);
void *readdir_fake(void *dirp);
int closedir_fake(void *dirp);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);
int statfs_fake(const char *path, void *buf);
FILE *fopen_fake(const char *path, const char *mode);
FILE *fdopen_fake(int fd, const char *mode);

// locale
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
int iswalpha_l_fake(int wc, void *loc); int iswblank_l_fake(int wc, void *loc);
int iswcntrl_l_fake(int wc, void *loc); int iswdigit_l_fake(int wc, void *loc);
int iswlower_l_fake(int wc, void *loc); int iswprint_l_fake(int wc, void *loc);
int iswpunct_l_fake(int wc, void *loc); int iswspace_l_fake(int wc, void *loc);
int iswupper_l_fake(int wc, void *loc); int iswxdigit_l_fake(int wc, void *loc);
int towlower_l_fake(int wc, void *loc); int towupper_l_fake(int wc, void *loc);
int strcoll_l_fake(const char *a, const char *b, void *loc);
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc);
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc);
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int feof_fake(FILE *f);
int fileno_fake(FILE *f);
int fseek_fake(FILE *f, long off, int whence);
long ftell_fake(FILE *f);
char *fgets_fake(char *s, int n, FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap_fake(void *addr, size_t length);
int mprotect_fake(void *addr, size_t len, int prot);
int madvise_fake(void *addr, size_t len, int advice);

// fd routing (fake pipe vs real files)
long read_fake(int fd, void *buf, size_t count);
long z_lseek(int fd, long off, int whence);   /* real lseek; also services lseek64 */
long write_fake(int fd, const void *buf, size_t count);
int  close_fake(int fd);
int  pipe_fake(int fds[2]);
int  poll_fake(void *fds, unsigned long nfds, int timeout);
int  select_fake(int n, void *r, void *w, void *e, void *t);

// networking
int socket_fake(int d, int t, int p);
int connect_fake(int s, const void *a, unsigned l);
int bind_fake(int s, const void *a, unsigned l);
int listen_fake(int s, int b);
int accept_fake(int s, void *a, void *l);
long send_fake(int s, const void *b, size_t l, int f);
long recv_fake(int s, void *b, size_t l, int f);
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al);
int shutdown_fake(int s, int how);
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l);
int getsockopt_fake(int s, int lv, int n, void *v, void *l);
int getsockname_fake(int s, void *a, void *l);
int getpeername_fake(int s, void *a, void *l);
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
int fcntl_shim(int fd, int cmd, int arg);
extern int g_net_on;
long recvmsg_fake(int s, void *msg, int flags);
long sendmsg_fake(int s, const void *msg, int flags);
int inet_pton_shim(int af, const char *src, void *dst);
const char *inet_ntop_shim(int af, const void *src, char *dst, unsigned size);
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f);
int gethostname_fake(char *name, size_t len);
unsigned if_nametoindex_fake(const char *n);
int kill_fake(int pid, int sig);
int getpid_fake(void);
int sched_yield_fake(void);
void *getpwuid_fake(int uid);
char *getenv_fake(const char *name);
char *getcwd_fake(char *buf, size_t size);

// dynamic loader
void *dlopen_fake(const char *name, int flags);
int dlclose_fake(void *h);
const char *dlerror_fake(void);
void *dlsym_fake(void *handle, const char *symbol);

// pthread extras
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_timedwait_fake(void **s, const struct timespec *abs);
int sem_getvalue_fake(void **s, int *val);

/* IL2CPP GC signal acknowledgement. */
extern uintptr_t g_il2cpp_base;
int pthread_kill_gc(pthread_t t, int sig);

#endif
