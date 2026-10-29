// fs.h — LittleFS on the `storage` partition, mounted at /data. Holds config.json and
// config.lkg.json (docs/SPEC.md §6.1). LittleFS survives power loss mid-write.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FS_ROOT "/data"

// Mounts, formatting the partition the first time. False if it can't be mounted.
bool fs_init(void);

// Reads a whole file into a new NUL-terminated buffer (caller frees). NULL if missing or too big.
char *fs_read(const char *path, size_t max_len);

// Writes `data` to `path` via a temp file and rename, so a reset never leaves half a file.
bool fs_write_atomic(const char *path, const char *data, size_t len);

bool fs_exists(const char *path);
bool fs_rename(const char *from, const char *to);   // replaces `to`
bool fs_remove(const char *path);
