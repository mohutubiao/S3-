#ifndef DAFEIYU_STORAGE_H
#define DAFEIYU_STORAGE_H

#include <cstddef>
#include <cstdint>

namespace dafeiyu {

// 壁纸按 RGB565 原始像素存，不放 JPEG：设备端就不用挂解码器，
// 读取后直接给 LVGL 当 LV_COLOR_FORMAT_RGB565 用
constexpr int kWallpaperW = 320;
constexpr int kWallpaperH = 240;
constexpr size_t kWallpaperBytes = static_cast<size_t>(kWallpaperW) * kWallpaperH * 2;

/** 挂载 storage 分区（没格式化过就自动格式化）。不能重复调用 */
bool MountStorage();

/** 是否已经上传过壁纸 */
bool HasWallpaper();

/** 读出壁纸到 out（需要 kWallpaperBytes 字节） */
bool LoadWallpaper(uint8_t* out, size_t size);

/** 写入壁纸（data 长度必须是 kWallpaperBytes） */
bool SaveWallpaper(const uint8_t* data, size_t size);

/** 删掉壁纸，回落到纯色背景 */
bool DeleteWallpaper();

}  // namespace dafeiyu

#endif  // DAFEIYU_STORAGE_H
