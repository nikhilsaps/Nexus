// Extracted parser checks; no ESP-IDF dependency.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cstdio>
static bool ae_accepts_gzip(const char* ae) {
  bool gzip_seen = false, gzip_ok = false, star_seen = false, star_ok = false;
  const char* p = ae;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    if (!*p) break;
    const char* tok = p;
    while (*p && *p != ',') p++;
    const char* end = p;

    const char* semi = tok;
    while (semi < end && *semi != ';') semi++;
    const char* name_end = semi;
    while (name_end > tok && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
    size_t nlen = (size_t)(name_end - tok);

    double q = 1.0;
    for (const char* s = semi; s < end; s++) {
      if (*s != ';') continue;
      const char* a = s + 1;
      while (a < end && (*a == ' ' || *a == '\t')) a++;
      if (a < end && (*a == 'q' || *a == 'Q')) {
        const char* eq = a + 1;
        while (eq < end && (*eq == ' ' || *eq == '\t')) eq++;
        if (eq < end && *eq == '=') q = strtod(eq + 1, NULL);
      }
    }

    if (nlen == 4 && !strncasecmp(tok, "gzip", 4)) { gzip_seen = true; gzip_ok = q > 0.0; }
    else if (nlen == 1 && tok[0] == '*')           { star_seen = true; star_ok = q > 0.0; }
  }
  if (gzip_seen) return gzip_ok;
  return star_seen && star_ok;
}


int main() {
 assert(ae_accepts_gzip("gzip"));
 assert(ae_accepts_gzip("br, gzip;q=0.5"));
 assert(!ae_accepts_gzip("gzip;q=0, *;q=1"));
 assert(!ae_accepts_gzip("br, deflate"));
 assert(!ae_accepts_gzip(""));
 assert(ae_accepts_gzip("*;q=1"));
 assert(!ae_accepts_gzip("*;q=0"));
 assert(ae_accepts_gzip("GZIP; q=1"));
 puts("PASS: eight gzip negotiation cases");
}
