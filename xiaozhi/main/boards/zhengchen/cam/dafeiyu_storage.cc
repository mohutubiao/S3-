#include "dafeiyu_storage.h"

#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_vfs.h>

#include <cstdio>
#include <cstring>

#define TAG "DafeiyuStorage"

namespace dafeiyu {

namespace {
constexpr const char* kBasePath = "/storage";
constexpr const char* kPartition = "storage";
constexpr const char* kWallpaperPath = "/storage/wallpaper.bin";
bool mounted_ = false;
}  // namespace

bool MountStorage() {
    if (mounted_) {
        return true;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kBasePath,
        .partition_label = kPartition,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s", kPartition, esp_err_to_name(ret));
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "storage mounted: %u KB total, %u KB used", (unsigned)(total / 1024),
             (unsigned)(used / 1024));
    mounted_ = true;
    return true;
}

bool HasWallpaper() {
    if (!mounted_) {
        return false;
    }
    struct stat st;
    return stat(kWallpaperPath, &st) == 0 && st.st_size == (long)kWallpaperBytes;
}

bool LoadWallpaper(uint8_t* out, size_t size) {
    if (!mounted_ || size < kWallpaperBytes) {
        return false;
    }
    FILE* f = fopen(kWallpaperPath, "rb");
    if (f == nullptr) {
        return false;
    }
    size_t got = fread(out, 1, kWallpaperBytes, f);
    fclose(f);
    if (got != kWallpaperBytes) {
        ESP_LOGW(TAG, "wallpaper size mismatch: %u != %u", (unsigned)got,
                 (unsigned)kWallpaperBytes);
        return false;
    }
    return true;
}

bool SaveWallpaper(const uint8_t* data, size_t size) {
    if (!mounted_) {
        return false;
    }
    if (size != kWallpaperBytes) {
        ESP_LOGW(TAG, "bad wallpaper size %u (expect %u)", (unsigned)size,
                 (unsigned)kWallpaperBytes);
        return false;
    }
    /* ⚠️ 2026-11-06 修复「上传显示写入失败」：SPIFFS 是日志式文件系统，
       旧壁纸还占着 150KB 时再写同名 150KB 文件，384KB 分区直接空间不足
       fopen/写失败。先删旧的再写；还失败就格式化分区重来
       （storage 分区里只有壁纸这一份用户数据，格式化无损）。 */
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            /* storage 分区里只有壁纸这一份用户数据，格式化无损 */
            ESP_LOGW(TAG, "write failed, reformatting storage...");
            esp_spiffs_format(kPartition);
        } else {
            remove(kWallpaperPath);
        }
        FILE* f = fopen(kWallpaperPath, "wb");
        if (f == nullptr) {
            ESP_LOGE(TAG, "cannot write wallpaper (attempt %d)", attempt);
            continue;
        }
        size_t wrote = fwrite(data, 1, size, f);
        fclose(f);
        if (wrote != size) {
            ESP_LOGE(TAG, "wallpaper write short: %u (attempt %d)", (unsigned)wrote, attempt);
            remove(kWallpaperPath);   // 删掉残片再重试
            continue;
        }
        size_t total = 0, used = 0;
        esp_spiffs_info(kPartition, &total, &used);
        ESP_LOGI(TAG, "wallpaper saved (%u bytes, storage %u/%u KB)", (unsigned)size,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
        return true;
    }
    return false;
}

bool DeleteWallpaper() {
    if (!mounted_) {
        return false;
    }
    return remove(kWallpaperPath) == 0;
}

}  // namespace dafeiyu
