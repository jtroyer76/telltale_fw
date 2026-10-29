#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

static bool s_mounted;

bool fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_ROOT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "mounted %s, %u of %u bytes used", FS_ROOT, (unsigned)used, (unsigned)total);
    s_mounted = true;
    return true;
}

char *fs_read(const char *path, size_t max_len)
{
    if (!s_mounted) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = NULL;
    if (len >= 0 && (size_t)len <= max_len && (buf = malloc(len + 1))) {
        if (fread(buf, 1, len, f) == (size_t)len) {
            buf[len] = '\0';
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

bool fs_write_atomic(const char *path, const char *data, size_t len)
{
    if (!s_mounted) return false;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    return fs_rename(tmp, path);
}

bool fs_exists(const char *path)
{
    struct stat st;
    return s_mounted && stat(path, &st) == 0;
}

bool fs_rename(const char *from, const char *to)
{
    unlink(to);   // POSIX rename replaces, but don't rely on the VFS doing it
    return rename(from, to) == 0;
}

bool fs_remove(const char *path) { return unlink(path) == 0; }
