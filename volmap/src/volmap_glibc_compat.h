/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * volmap_glibc_compat.h - pin symbol versions so a binary built on a new glibc
 *                         still runs on an old one.
 *
 * Why this is needed
 *   glibc 2.34 merged libdl and libpthread into libc and re-versioned the symbols
 *   that moved.  A build on 2.34+ therefore records GLIBC_2.34 for dlopen,
 *   pthread_create and friends, and the binary refuses to start on anything older
 *   even though the functions themselves have existed since 2.2.5.
 *
 *   The old versions are still present in the same libc, so asking the linker for
 *   them explicitly produces a binary that runs on both.  Nothing is reimplemented
 *   here - the same libc code runs, only the recorded version differs.
 *
 * How to use
 *   Compile with -DVOLMAP_GLIBC_COMPAT (the build scripts add it automatically when
 *   the host glibc is 2.34 or newer).  Without it this header does nothing, so a
 *   build on an old glibc is unaffected.
 *
 * Scope
 *   x86-64 only.  The .symver directives name the x86-64 ABI versions; on another
 *   architecture the macro is ignored and the build behaves as before.
 */

#ifndef _VOLMAP_GLIBC_COMPAT_H_
#define _VOLMAP_GLIBC_COMPAT_H_

#if defined(VOLMAP_GLIBC_COMPAT) && defined(__GLIBC__) && defined(__x86_64__)

/* Moved out of libdl/libpthread in 2.34; the 2.2.5 versions remain in libc. */
__asm__ (".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__ (".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__ (".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__ (".symver dlerror,dlerror@GLIBC_2.2.5");
__asm__ (".symver pthread_create,pthread_create@GLIBC_2.2.5");
__asm__ (".symver pthread_join,pthread_join@GLIBC_2.2.5");
__asm__ (".symver pthread_detach,pthread_detach@GLIBC_2.2.5");

/* Re-versioned for other reasons; the old entry points are still exported. */
__asm__ (".symver memcpy,memcpy@GLIBC_2.2.5");      /* 2.14 added an SSE variant   */
__asm__ (".symver clock_gettime,clock_gettime@GLIBC_2.2.5");  /* moved from librt in 2.17 */
__asm__ (".symver log2,log2@GLIBC_2.2.5");          /* 2.29 added a faster version */

/* stat() is different: before 2.33 glibc exposed no stat symbol at all - the header
   inlined a call to __xstat(ver, path, buf).  There is no older stat@ to ask for, so
   the call is routed to __xstat, which is still exported at 2.2.5.  _STAT_VER is the
   x86-64 value (1); it is what the old inline passed. */
#include <sys/stat.h>
/* volmap.c is compiled as C++ (-x c++), so these must not be name-mangled. */
#ifdef __cplusplus
extern "C" {
#endif
extern int __xstat (int, const char *, struct stat *);
extern int __lxstat (int, const char *, struct stat *);
extern int __fxstat (int, int, struct stat *);
#ifdef __cplusplus
}
#endif
#ifndef _STAT_VER
#define _STAT_VER 1
#endif
static inline int
volmap_compat_stat (const char *path, struct stat *buf)
{
  return __xstat (_STAT_VER, path, buf);
}
static inline int
volmap_compat_lstat (const char *path, struct stat *buf)
{
  return __lxstat (_STAT_VER, path, buf);
}
static inline int
volmap_compat_fstat (int fd, struct stat *buf)
{
  return __fxstat (_STAT_VER, fd, buf);
}
#define stat(p_, b_)  volmap_compat_stat ((p_), (b_))
#define lstat(p_, b_) volmap_compat_lstat ((p_), (b_))
#define fstat(f_, b_) volmap_compat_fstat ((f_), (b_))

#endif /* VOLMAP_GLIBC_COMPAT && __GLIBC__ && __x86_64__ */

#endif /* _VOLMAP_GLIBC_COMPAT_H_ */
