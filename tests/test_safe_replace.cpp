// test_safe_replace.cpp - host tests for main/safe_replace.h.
//
// The header routes every filesystem call through an NX_SR_* macro, so the
// whole replace sequence can be driven against an in-memory FAT-like fake with
// faults injected at any single step. Build: see tests/Makefile.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <set>
#include <string>

// ----------------------------------------------------------- fake filesystem

static std::map<std::string, std::string> g_fs;    // path -> contents
static std::set<std::string> g_fail_rename_src;    // rename(src, *) fails
static std::set<std::string> g_fail_remove;        // remove(path) fails
static std::set<std::string> g_fail_open;          // open(path) fails (EIO)
static bool g_fdopen_fail = false;
static bool g_fflush_fail = false;
static bool g_ferror_flag = false;
static bool g_fclose_fail = false;

struct FakeFile { std::string path; };

static int  fake_open(const char* path, int flags, ...);
static FILE* fake_fdopen(int fd, const char* mode);
static int  fake_close(int fd);
static int  fake_fflush(FILE* f);
static int  fake_ferror(FILE* f);
static int  fake_fclose(FILE* f);
static int  fake_rename(const char* from, const char* to);
static int  fake_remove(const char* path);
static int  fake_exists(const char* path);

#define NX_SR_OPEN   fake_open
#define NX_SR_FDOPEN fake_fdopen
#define NX_SR_CLOSE  fake_close
#define NX_SR_FFLUSH fake_fflush
#define NX_SR_FERROR fake_ferror
#define NX_SR_FCLOSE fake_fclose
#define NX_SR_RENAME fake_rename
#define NX_SR_REMOVE fake_remove
#define NX_SR_EXISTS fake_exists

#include "safe_replace.h"

static std::map<int, FakeFile> g_fds;
static int g_next_fd = 3;

static int fake_open(const char* path, int flags, ...) {
  if (g_fail_open.count(path)) { errno = EIO; return -1; }
  if ((flags & O_EXCL) && g_fs.count(path)) { errno = EEXIST; return -1; }
  g_fs[path] = "";
  int fd = g_next_fd++;
  g_fds[fd] = FakeFile{path};
  return fd;
}

static FILE* fake_fdopen(int fd, const char*) {
  if (g_fdopen_fail) { errno = ENOMEM; return NULL; }
  FakeFile* ff = new FakeFile(g_fds[fd]);
  return reinterpret_cast<FILE*>(ff);
}

static int fake_close(int fd) { g_fds.erase(fd); return 0; }

static int fake_fflush(FILE*) { return g_fflush_fail ? EOF : 0; }
static int fake_ferror(FILE*) { return g_ferror_flag ? 1 : 0; }

static int fake_fclose(FILE* f) {
  delete reinterpret_cast<FakeFile*>(f);
  return g_fclose_fail ? EOF : 0;
}

// FATFS rename() refuses to overwrite an existing name; the fake must too, or
// the tests would pass against a sequence that only works on POSIX.
static int fake_rename(const char* from, const char* to) {
  if (g_fail_rename_src.count(from)) { errno = EIO; return -1; }
  auto it = g_fs.find(from);
  if (it == g_fs.end()) { errno = ENOENT; return -1; }
  if (g_fs.count(to))   { errno = EEXIST; return -1; }
  g_fs[to] = it->second;
  g_fs.erase(it);
  return 0;
}

static int fake_remove(const char* path) {
  if (g_fail_remove.count(path)) { errno = EIO; return -1; }
  if (!g_fs.count(path)) { errno = ENOENT; return -1; }
  g_fs.erase(path);
  return 0;
}

static int fake_exists(const char* path) { return g_fs.count(path) ? 1 : 0; }

static void fs_reset() {
  g_fs.clear(); g_fds.clear();
  g_fail_rename_src.clear(); g_fail_remove.clear(); g_fail_open.clear();
  g_fdopen_fail = g_fflush_fail = g_ferror_flag = g_fclose_fail = false;
  errno = 0;
}

// ----------------------------------------------------------------- harness

static int g_fail = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#define CHECK_EQ_STR(got, want) do { g_checks++; if (std::string(got) != std::string(want)) { \
    printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
           std::string(got).c_str(), std::string(want).c_str()); g_fail++; } } while (0)

static const char* DST = "/sdcard/sites/a.html";
static const char* TMP = "/sdcard/sites/a.html.nexus-tmp";
static const char* BAK = "/sdcard/sites/a.html.nexus-bak";

static bool has(const char* p) { return g_fs.count(p) != 0; }
static std::string body(const char* p) { auto it = g_fs.find(p); return it == g_fs.end() ? "<absent>" : it->second; }

// ------------------------------------------------------------ replace tests

static void test_replace_over_existing() {
  fs_reset();
  g_fs[DST] = "old"; g_fs[TMP] = "new";
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "new");
  CHECK(!has(TMP));
  CHECK(!has(BAK));
}

static void test_replace_fresh_target() {
  fs_reset();
  g_fs[TMP] = "new";
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "new");
  CHECK(!has(TMP));
  CHECK(!has(BAK));
}

// The regression the whole header exists for: an existing backup is the only
// copy of the original after an interrupted save, so it must be refused, never
// cleared to make room.
static void test_replace_refuses_existing_backup() {
  fs_reset();
  g_fs[BAK] = "the only original"; g_fs[TMP] = "new";
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_STALE_BAK);
  CHECK_EQ_STR(body(BAK), "the only original");
  CHECK(!has(DST));
  CHECK_EQ_STR(body(TMP), "new");          // caller still owns the temp file
}

static void test_replace_refuses_backup_even_with_target() {
  fs_reset();
  g_fs[DST] = "live"; g_fs[BAK] = "older"; g_fs[TMP] = "new";
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_STALE_BAK);
  CHECK_EQ_STR(body(DST), "live");
  CHECK_EQ_STR(body(BAK), "older");
}

static void test_replace_backup_rename_fails() {
  fs_reset();
  g_fs[DST] = "old"; g_fs[TMP] = "new";
  g_fail_rename_src.insert(DST);
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_BACKUP);
  CHECK_EQ_STR(body(DST), "old");          // original untouched
  CHECK_EQ_STR(body(TMP), "new");
  CHECK(!has(BAK));
}

static void test_replace_commit_fails_and_restores() {
  fs_reset();
  g_fs[DST] = "old"; g_fs[TMP] = "new";
  g_fail_rename_src.insert(TMP);
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_RENAME);
  CHECK_EQ_STR(body(DST), "old");          // restored from the sidecar
  CHECK(!has(BAK));
  CHECK_EQ_STR(body(TMP), "new");
}

static void test_replace_commit_and_restore_both_fail() {
  fs_reset();
  g_fs[DST] = "old"; g_fs[TMP] = "new";
  g_fail_rename_src.insert(TMP);
  g_fail_rename_src.insert(BAK);
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_RENAME_BAK_KEPT);
  CHECK_EQ_STR(body(BAK), "old");          // original survives, under the sidecar
  CHECK(!has(DST));
  CHECK_EQ_STR(body(TMP), "new");          // and nothing was deleted
}

static void test_replace_leftover_backup_blocks_next_save() {
  fs_reset();
  g_fs[DST] = "old"; g_fs[TMP] = "new";
  g_fail_remove.insert(BAK);               // commit succeeds, cleanup does not
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "new");
  CHECK_EQ_STR(body(BAK), "old");
  g_fs[TMP] = "newer";
  CHECK(nx_sr_replace(TMP, DST, BAK) == NX_SR_E_STALE_BAK);
  CHECK_EQ_STR(body(BAK), "old");          // refused, not cleared
}

static void test_replace_null_args() {
  fs_reset();
  CHECK(nx_sr_replace(NULL, DST, BAK) == NX_SR_E_ARG);
  CHECK(nx_sr_replace(TMP, NULL, BAK) == NX_SR_E_ARG);
  CHECK(nx_sr_replace(TMP, DST, NULL) == NX_SR_E_ARG);
}

// ------------------------------------------------------------ recover tests

static void test_recover_noop() {
  fs_reset();
  g_fs[DST] = "live";
  CHECK(nx_sr_recover(DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "live");
}

static void test_recover_restores_orphan_backup() {
  fs_reset();
  g_fs[BAK] = "orig";
  CHECK(nx_sr_recover(DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "orig");
  CHECK(!has(BAK));
}

static void test_recover_refuses_when_ambiguous() {
  fs_reset();
  g_fs[DST] = "live"; g_fs[BAK] = "orig";
  CHECK(nx_sr_recover(DST, BAK) == NX_SR_E_STALE_BAK);
  CHECK_EQ_STR(body(DST), "live");
  CHECK_EQ_STR(body(BAK), "orig");         // human decides; nothing deleted
}

static void test_recover_rename_fails() {
  fs_reset();
  g_fs[BAK] = "orig";
  g_fail_rename_src.insert(BAK);
  CHECK(nx_sr_recover(DST, BAK) == NX_SR_E_BACKUP);
  CHECK_EQ_STR(body(BAK), "orig");
}

// --------------------------------------------- save-handler commit policy
// Mirrors the three-step commit in save_handler() (localserve.cpp): replace,
// and on a refused sidecar try the non-destructive recovery once.

static nx_sr_result_t save_commit(const char* tmp, const char* dst, const char* bak) {
  nx_sr_result_t r = nx_sr_replace(tmp, dst, bak);
  if (r == NX_SR_E_STALE_BAK && nx_sr_recover(dst, bak) == NX_SR_OK) {
    r = nx_sr_replace(tmp, dst, bak);
  }
  return r;
}

static void test_save_after_interrupted_save_recovers() {
  fs_reset();
  g_fs[BAK] = "orig"; g_fs[TMP] = "new";   // previous attempt died mid-sequence
  CHECK(save_commit(TMP, DST, BAK) == NX_SR_OK);
  CHECK_EQ_STR(body(DST), "new");
  CHECK(!has(BAK));
  CHECK(!has(TMP));
}

static void test_save_refuses_when_backup_ambiguous() {
  fs_reset();
  g_fs[DST] = "live"; g_fs[BAK] = "orig"; g_fs[TMP] = "new";
  CHECK(save_commit(TMP, DST, BAK) == NX_SR_E_STALE_BAK);
  CHECK_EQ_STR(body(DST), "live");
  CHECK_EQ_STR(body(BAK), "orig");
}

// ------------------------------------------------------- open / close tests

static void test_open_exclusive_creates() {
  fs_reset();
  FILE* f = nx_sr_open_exclusive(TMP);
  CHECK(f != NULL);
  CHECK(has(TMP));
  CHECK(nx_sr_fclose_checked(f) == 1);
}

static void test_open_exclusive_refuses_collision() {
  fs_reset();
  g_fs[TMP] = "in flight";
  errno = 0;
  FILE* f = nx_sr_open_exclusive(TMP);
  CHECK(f == NULL);
  CHECK(errno == EEXIST);
  CHECK_EQ_STR(body(TMP), "in flight");    // the other writer's bytes survive
}

static void test_open_exclusive_fdopen_failure_cleans_up() {
  fs_reset();
  g_fdopen_fail = true;
  FILE* f = nx_sr_open_exclusive(TMP);
  CHECK(f == NULL);
  CHECK(!has(TMP));                        // no zero-byte temp left behind
}

static void test_open_exclusive_open_failure() {
  fs_reset();
  g_fail_open.insert(TMP);
  errno = 0;
  CHECK(nx_sr_open_exclusive(TMP) == NULL);
  CHECK(errno == EIO);
  CHECK(nx_sr_open_exclusive(NULL) == NULL);
}

static void test_fclose_checked_reports_each_failure() {
  fs_reset();
  g_fflush_fail = true;
  CHECK(nx_sr_fclose_checked(nx_sr_open_exclusive(TMP)) == 0);

  fs_reset();
  g_ferror_flag = true;
  CHECK(nx_sr_fclose_checked(nx_sr_open_exclusive(TMP)) == 0);

  fs_reset();
  g_fclose_fail = true;
  CHECK(nx_sr_fclose_checked(nx_sr_open_exclusive(TMP)) == 0);

  fs_reset();
  CHECK(nx_sr_fclose_checked(NULL) == 0);
}

// ------------------------------------------------------- path / sidecar rules

static void test_sidecar_detection() {
  CHECK(nx_sr_is_sidecar("/a.html.nexus-tmp"));
  CHECK(nx_sr_is_sidecar("/a.html.nexus-bak"));
  CHECK(nx_sr_is_sidecar("/a.html.NEXUS-BAK"));    // FAT is case-insensitive
  CHECK(!nx_sr_is_sidecar("/a.html"));
  CHECK(!nx_sr_is_sidecar("/nexus-bak.html"));
  CHECK(!nx_sr_is_sidecar(NULL));
}

static void test_rel_path_ok_accepts() {
  const char* ok[] = {
    "/index.html", "/site/app.js", "/a/b/c/d.png", "/weird..name.txt",
    "/dot.dir/file", "/\xc3\xa9t\xc3\xa9.html",
  };
  for (const char* p : ok) { g_checks++; if (!nx_sr_rel_path_ok(p, 1023)) {
    printf("FAIL rejected a legal path: %s\n", p); g_fail++; } }
}

static void test_rel_path_ok_rejects() {
  const char* bad[] = {
    "", "/", "index.html", "/../secret", "/a/../../b", "/a/./b", "/a//b",
    "/dir/", "/a\\b", "/a:b", "/a*b", "/a?b", "/a\"b", "/a<b", "/a>b", "/a|b",
    "/a\nb", "/a\x7f" "b", "/a\x01",
    "/a.html.nexus-tmp", "/a.html.nexus-bak", "/a.html.NeXuS-bAk",
    "/a.html.nexus-bak.", "/a.html.nexus-bak ", "/trailing.", "/trailing ",
    "/.. /x", "/dir./f",
  };
  for (const char* p : bad) { g_checks++; if (nx_sr_rel_path_ok(p, 1023)) {
    printf("FAIL accepted an illegal path: %s\n", p); g_fail++; } }
  CHECK(!nx_sr_rel_path_ok(NULL, 1023));
}

static void test_rel_path_ok_length_bound() {
  std::string p = "/" + std::string(40, 'a');
  CHECK(nx_sr_rel_path_ok(p.c_str(), p.size()));
  CHECK(!nx_sr_rel_path_ok(p.c_str(), p.size() - 1));
}

int main() {
  test_replace_over_existing();
  test_replace_fresh_target();
  test_replace_refuses_existing_backup();
  test_replace_refuses_backup_even_with_target();
  test_replace_backup_rename_fails();
  test_replace_commit_fails_and_restores();
  test_replace_commit_and_restore_both_fail();
  test_replace_leftover_backup_blocks_next_save();
  test_replace_null_args();

  test_recover_noop();
  test_recover_restores_orphan_backup();
  test_recover_refuses_when_ambiguous();
  test_recover_rename_fails();

  test_save_after_interrupted_save_recovers();
  test_save_refuses_when_backup_ambiguous();

  test_open_exclusive_creates();
  test_open_exclusive_refuses_collision();
  test_open_exclusive_fdopen_failure_cleans_up();
  test_open_exclusive_open_failure();
  test_fclose_checked_reports_each_failure();

  test_sidecar_detection();
  test_rel_path_ok_accepts();
  test_rel_path_ok_rejects();
  test_rel_path_ok_length_bound();

  printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
