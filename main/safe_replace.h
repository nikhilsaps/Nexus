// safe_replace.h - file-replacement helpers for the LocalServe save API.
//
// Header-only and free of ESP-IDF dependencies, so the same code runs on a
// host: every filesystem call goes through an NX_SR_* macro that a test can
// point at a fake (see tests/test_safe_replace.cpp).
//
// This is NOT power-loss atomic, and nothing here should be described as such.
// FATFS on the microSD gives no write-ordering or durability guarantee, so a
// reset in the middle of the sequence can leave the new bytes in the
// <target>.nexus-tmp sidecar or the old bytes in the <target>.nexus-bak
// sidecar. What the sequence does guarantee is that no single *failed* step
// destroys the original: the original is moved aside (not unlinked) before the
// replacement takes its name, and is restored if that move fails.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

// Reserved sidecar suffixes. Both are part of the on-card contract: a client
// may never read, write or replace a path that ends in one of them, because
// they are where this module parks in-flight and previous content.
#define NX_SR_TMP_SUFFIX ".nexus-tmp"
#define NX_SR_BAK_SUFFIX ".nexus-bak"
#define NX_SR_SUFFIX_MAX 16   // longest sidecar suffix incl. NUL, for sizing

// Injection points. Each defaults to the libc call of the same name.
#ifndef NX_SR_OPEN
#define NX_SR_OPEN open
#endif
#ifndef NX_SR_FDOPEN
#define NX_SR_FDOPEN fdopen
#endif
#ifndef NX_SR_CLOSE
#define NX_SR_CLOSE close
#endif
#ifndef NX_SR_FFLUSH
#define NX_SR_FFLUSH fflush
#endif
#ifndef NX_SR_FERROR
#define NX_SR_FERROR ferror
#endif
#ifndef NX_SR_FCLOSE
#define NX_SR_FCLOSE fclose
#endif
#ifndef NX_SR_RENAME
#define NX_SR_RENAME rename
#endif
#ifndef NX_SR_REMOVE
#define NX_SR_REMOVE remove
#endif
#ifndef NX_SR_STAT
#define NX_SR_STAT stat
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NX_SR_OK = 0,
  NX_SR_E_ARG,            // NULL argument
  NX_SR_E_STALE_BAK,      // a backup sidecar already exists; refused, nothing moved
  NX_SR_E_BACKUP,         // could not move the original aside; original untouched
  NX_SR_E_RENAME,         // temp -> target failed; target holds the original
  NX_SR_E_RENAME_BAK_KEPT // temp -> target failed AND restore failed; the
                          // original is parked in the backup sidecar
} nx_sr_result_t;

static inline const char* nx_sr_strerr(nx_sr_result_t r) {
  switch (r) {
    case NX_SR_OK:                return "ok";
    case NX_SR_E_ARG:             return "bad argument";
    case NX_SR_E_STALE_BAK:       return "backup sidecar exists, needs manual recovery";
    case NX_SR_E_BACKUP:          return "cannot move original aside";
    case NX_SR_E_RENAME:          return "rename failed, original kept";
    case NX_SR_E_RENAME_BAK_KEPT: return "rename failed, original left in backup sidecar";
  }
  return "unknown";
}

#ifndef NX_SR_EXISTS
static inline int nx_sr_default_exists(const char* path) {
  struct stat st;
  return NX_SR_STAT(path, &st) == 0;
}
#define NX_SR_EXISTS nx_sr_default_exists
#endif

static inline int nx_sr_lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

// FAT filenames are case-insensitive, so the suffix match has to be too.
static inline int nx_sr_has_suffix_ci(const char* s, const char* suffix) {
  if (!s || !suffix) return 0;
  size_t sl = strlen(s), fl = strlen(suffix);
  if (fl == 0 || sl < fl) return 0;
  const char* tail = s + (sl - fl);
  for (size_t i = 0; i < fl; i++) {
    if (nx_sr_lower((unsigned char)tail[i]) != nx_sr_lower((unsigned char)suffix[i])) return 0;
  }
  return 1;
}

// True for any path the save machinery reserves for itself.
static inline int nx_sr_is_sidecar(const char* path) {
  return nx_sr_has_suffix_ci(path, NX_SR_TMP_SUFFIX) ||
         nx_sr_has_suffix_ci(path, NX_SR_BAK_SUFFIX);
}

// Validate a client-supplied, slash-rooted relative path before it is pasted
// under a server root. Rejects: anything not starting with '/', over-long
// paths, control characters, the FAT-hostile set (including the '\' separator
// and ':'), empty / "." / ".." components, components with a trailing dot or
// space, a trailing slash, and reserved sidecar names. `max_len` is the
// longest strlen() accepted.
//
// Trailing dots and spaces matter beyond tidiness: FAT/Windows name resolution
// strips them, so "/a.nexus-bak." and "/../" written as "/.. /" would alias
// names that the sidecar and traversal checks just rejected.
static inline int nx_sr_rel_path_ok(const char* rel, size_t max_len) {
  if (!rel || rel[0] != '/') return 0;
  size_t len = strlen(rel);
  if (len < 2 || len > max_len) return 0;
  if (rel[len - 1] == '/') return 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)rel[i];
    if (c < 0x20 || c == 0x7f) return 0;
    if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') return 0;
  }
  size_t start = 1;
  for (size_t i = 1; i <= len; i++) {
    if (i == len || rel[i] == '/') {
      size_t clen = i - start;
      if (clen == 0) return 0;
      if (clen == 1 && rel[start] == '.') return 0;
      if (clen == 2 && rel[start] == '.' && rel[start + 1] == '.') return 0;
      if (rel[i - 1] == '.' || rel[i - 1] == ' ') return 0;   // FAT strips these
      start = i + 1;
    }
  }
  return !nx_sr_is_sidecar(rel);
}

// Create `path` for writing, failing if it already exists (errno == EEXIST).
// Exclusive creation is what makes a concurrent save a detectable collision
// instead of two writers interleaving into one temp file.
static inline FILE* nx_sr_open_exclusive(const char* path) {
  if (!path) { errno = EINVAL; return NULL; }
  int fd = NX_SR_OPEN(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
  if (fd < 0) return NULL;
  FILE* f = NX_SR_FDOPEN(fd, "wb");
  if (!f) {
    int saved = errno;
    NX_SR_CLOSE(fd);
    NX_SR_REMOVE(path);
    errno = saved;
    return NULL;
  }
  return f;
}

// Flush, check the stream error flag, then close - and report any of the three
// failing. A plain fclose() hides both the deferred write error and the error
// that only surfaces when the last buffer is handed to the filesystem.
static inline int nx_sr_fclose_checked(FILE* f) {
  if (!f) return 0;
  int ok = 1;
  if (NX_SR_FFLUSH(f) != 0) ok = 0;
  if (NX_SR_FERROR(f)) ok = 0;
  if (NX_SR_FCLOSE(f) != 0) ok = 0;
  return ok;
}

// Recover from an interrupted replace, without ever deleting data: if the
// backup sidecar is the only surviving copy (target missing), move it back
// under the target name. If both exist the situation is ambiguous - which copy
// is wanted is a human decision - so it is refused and left untouched.
static inline nx_sr_result_t nx_sr_recover(const char* dst, const char* bak) {
  if (!dst || !bak) return NX_SR_E_ARG;
  if (!NX_SR_EXISTS(bak)) return NX_SR_OK;            // nothing to recover
  if (NX_SR_EXISTS(dst)) return NX_SR_E_STALE_BAK;    // ambiguous: keep both
  return NX_SR_RENAME(bak, dst) == 0 ? NX_SR_OK : NX_SR_E_BACKUP;
}

// Move `tmp` onto `dst`, keeping the old `dst` reachable the whole time:
//   dst -> bak -> tmp -> dst -> drop bak
// FATFS rename() refuses to overwrite, which is exactly why `dst` is moved to
// `bak` rather than removed. On failure the caller still owns `tmp`.
//
// An existing backup sidecar is REFUSED, never cleared: after a failed restore
// or a reset mid-sequence it can be the only copy of the original, and
// deleting it to make room would be precisely the data loss this sequence
// exists to prevent. Call nx_sr_recover() first to resolve one safely.
static inline nx_sr_result_t nx_sr_replace(const char* tmp, const char* dst, const char* bak) {
  if (!tmp || !dst || !bak) return NX_SR_E_ARG;

  if (NX_SR_EXISTS(bak)) return NX_SR_E_STALE_BAK;

  int had_bak = 0;
  if (NX_SR_EXISTS(dst)) {
    if (NX_SR_RENAME(dst, bak) != 0) return NX_SR_E_BACKUP;
    had_bak = 1;
  }

  if (NX_SR_RENAME(tmp, dst) != 0) {
    if (had_bak && NX_SR_RENAME(bak, dst) != 0) return NX_SR_E_RENAME_BAK_KEPT;
    return NX_SR_E_RENAME;
  }

  // Committed: the superseded copy is dropped. If this fails the sidecar
  // survives and the next replace refuses rather than clearing it.
  if (had_bak) NX_SR_REMOVE(bak);
  return NX_SR_OK;
}

#ifdef __cplusplus
}
#endif
