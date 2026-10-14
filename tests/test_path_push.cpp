// Generated from main.cpp path_push for executable boundary checks.
#include <cassert>
#include <cstring>
#include <cstdio>
static char g_path[256];
static bool path_push(const char* name) {
  size_t n = strlen(g_path);
  size_t slash = (n == 0 || g_path[n - 1] != '/') ? 1 : 0;
  size_t len = strlen(name);
  if (n + slash + len >= sizeof(g_path)) return false;
  if (slash) g_path[n++] = '/';
  memcpy(g_path + n, name, len + 1);
  return true;
}

int main() {
  strcpy(g_path, "/"); assert(path_push("folder")); assert(!strcmp(g_path, "/folder"));
  strcpy(g_path, "/folder"); assert(path_push("child")); assert(!strcmp(g_path, "/folder/child"));
  memset(g_path, 'a', 253); g_path[253]=0; assert(path_push("x")); assert(strlen(g_path)==255);
  char before[256]; memcpy(before,g_path,256); assert(!path_push("x")); assert(!memcmp(before,g_path,256));
  strcpy(g_path, "/"); char huge[300]; memset(huge,'z',299); huge[299]=0;
  assert(!path_push(huge)); assert(!strcmp(g_path,"/"));
  puts("PASS: root, nested, exact fit, overflow unchanged, oversized name unchanged");
}
