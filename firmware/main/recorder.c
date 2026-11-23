#include "recorder.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devlog.h"
#include "nvs.h"

#define KEEP 12

static int seq_of(const char *name) {
  if (strncmp(name, "rec-", 4)) return -1;
  return atoi(name + 4);
}

static void prune(void) {
  for (;;) {
    DIR *d = opendir("/data");
    if (!d) return;
    int n = 0, oldest = -1;
    char oldname[64] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
      int s = seq_of(e->d_name);
      if (s < 0) continue;
      n++;
      if (oldest < 0 || s < oldest) {
        oldest = s;
        snprintf(oldname, sizeof(oldname), "%s", e->d_name);
      }
    }
    closedir(d);
    if (n <= KEEP) return;
    char p[80];
    snprintf(p, sizeof(p), "/data/%s", oldname);
    unlink(p);
  }
}

FILE *rec_open(const char *via, char *name_out, size_t cap) {
  uint32_t seq = 0;
  nvs_handle_t h;
  if (nvs_open("rec", NVS_READWRITE, &h) == ESP_OK) {
    nvs_get_u32(h, "seq", &seq);
    seq++;
    nvs_set_u32(h, "seq", seq);
    nvs_commit(h);
    nvs_close(h);
  }
  snprintf(name_out, cap, "rec-%04lu-%s.jsonl", (unsigned long)seq, via);
  char p[96];
  snprintf(p, sizeof(p), "/data/%s", name_out);
  FILE *f = fopen(p, "w");
  if (!f) devlog("rec: cannot create %s", p);
  else devlog("rec: recording to %s", name_out);
  prune();
  return f;
}

void rec_close(FILE *f) {
  if (f) fclose(f);
}

cJSON *rec_list_json(void) {
  cJSON *arr = cJSON_CreateArray();
  DIR *d = opendir("/data");
  if (!d) return arr;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (seq_of(e->d_name) < 0) continue;
    char p[96];
    snprintf(p, sizeof(p), "/data/%s", e->d_name);
    struct stat sb;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e->d_name);
    cJSON_AddNumberToObject(o, "seq", seq_of(e->d_name));
    cJSON_AddNumberToObject(o, "bytes", stat(p, &sb) == 0 ? sb.st_size : 0);
    cJSON_AddItemToArray(arr, o);
  }
  closedir(d);
  return arr;
}

const char *rec_path(const char *name, char *buf, size_t cap) {
  if (seq_of(name) < 0 || strchr(name, '/') || strstr(name, "..")) return NULL;
  snprintf(buf, cap, "/data/%s", name);
  return buf;
}

int rec_delete(const char *name) {
  char p[96];
  if (!rec_path(name, p, sizeof(p))) return -1;
  return unlink(p);
}

void rec_latest(char *out, size_t cap) {
  out[0] = 0;
  int best = -1;
  DIR *d = opendir("/data");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    int s = seq_of(e->d_name);
    if (s > best) {
      best = s;
      snprintf(out, cap, "%s", e->d_name);
    }
  }
  closedir(d);
}
