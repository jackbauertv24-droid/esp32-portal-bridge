#include "urlutil.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int url_split(const char *url, url_parts_t *out) {
  memset(out, 0, sizeof(*out));
  while (*url == ' ') url++;
  const char *sep = strstr(url, "://");
  if (!sep) return -1;
  size_t sl = (size_t)(sep - url);
  if (sl >= sizeof(out->scheme)) return -1;
  for (size_t i = 0; i < sl; i++) out->scheme[i] = (char)tolower((unsigned char)url[i]);
  if (strcmp(out->scheme, "http") && strcmp(out->scheme, "https")) return -1;
  const char *h = sep + 3;
  size_t hl = strcspn(h, "/?#");
  if (!hl || hl >= sizeof(out->host)) return -1;
  for (size_t i = 0; i < hl; i++) out->host[i] = (char)tolower((unsigned char)h[i]);
  const char *rest = h + hl;
  size_t rl = strcspn(rest, "#");
  if (rl + 2 > sizeof(out->path)) rl = sizeof(out->path) - 2;
  if (rest[0] != '/') {
    out->path[0] = '/';
    memcpy(out->path + 1, rest, rl);
    out->path[rl + 1] = 0;
  } else {
    memcpy(out->path, rest, rl);
    out->path[rl] = 0;
  }
  return 0;
}

// Removes "." and ".." segments from a path (query left alone). `path` holds 1024 bytes.
static void normalize(char *path) {
  size_t ql = strcspn(path, "?");
  char query[1024];
  snprintf(query, sizeof(query), "%s", path + ql);
  path[ql] = 0;
  // Segment start offsets into `out`, used as a stack for "..".
  char out[1024];
  size_t o = 0, stack[64];
  int depth = 0;
  const char *p = path;
  size_t pl0 = strlen(path);
  int trailing_slash = (pl0 && path[pl0 - 1] == '/') || (pl0 >= 2 && !strcmp(path + pl0 - 2, "/.")) ||
                       (pl0 >= 3 && !strcmp(path + pl0 - 3, "/.."));
  while (*p) {
    while (*p == '/') p++;
    const char *seg = p;
    while (*p && *p != '/') p++;
    size_t len = (size_t)(p - seg);
    if (!len || (len == 1 && seg[0] == '.')) continue;
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      if (depth) o = stack[--depth];
      continue;
    }
    if (o + len + 2 >= sizeof(out) || depth >= 64) break;
    stack[depth++] = o;
    out[o++] = '/';
    memcpy(out + o, seg, len);
    o += len;
  }
  if (o == 0 || trailing_slash) out[o++] = '/';
  out[o] = 0;
  size_t room = 1024 - 1;
  size_t ol = strlen(out);
  if (ol > room) ol = room;
  memcpy(path, out, ol);
  size_t qn = strlen(query);
  if (ol + qn > room) qn = room - ol;
  memcpy(path + ol, query, qn);
  path[ol + qn] = 0;
}

int url_resolve(const char *base, const char *ref, char *out, size_t cap) {
  url_parts_t b;
  if (url_split(base, &b)) return -1;
  while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
  url_parts_t r;
  if (strstr(ref, "://") && url_split(ref, &r) == 0) {
    normalize(r.path);
    snprintf(out, cap, "%s://%s%s", r.scheme, r.host, r.path);
    return 0;
  }
  if (!strncasecmp(ref, "javascript:", 11) || !strncasecmp(ref, "mailto:", 7)) return -1;
  if (ref[0] == '/' && ref[1] == '/') {
    snprintf(out, cap, "%s:%s", b.scheme, ref);
    url_parts_t t;
    if (url_split(out, &t)) return -1;
    normalize(t.path);
    snprintf(out, cap, "%s://%s%s", t.scheme, t.host, t.path);
    return 0;
  }
  char path[1024];
  if (ref[0] == 0 || ref[0] == '#') {
    snprintf(path, sizeof(path), "%s", b.path);
  } else if (ref[0] == '/') {
    snprintf(path, sizeof(path), "%s", ref);
  } else if (ref[0] == '?') {
    size_t pl = strcspn(b.path, "?");
    snprintf(path, sizeof(path), "%.*s%s", (int)pl, b.path, ref);
  } else {
    size_t pl = strcspn(b.path, "?");
    size_t dir = pl;
    while (dir > 0 && b.path[dir - 1] != '/') dir--;
    snprintf(path, sizeof(path), "%.*s%s", (int)dir, b.path, ref);
  }
  path[strcspn(path, "#")] = 0;
  normalize(path);
  snprintf(out, cap, "%s://%s%s", b.scheme, b.host, path);
  return 0;
}
