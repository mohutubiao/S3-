#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();
    /* 2026-11-10：立即落盘。⚠️ SetInt/SetString 只标 dirty_，
       真正 nvs_commit 在析构里 —— 写完就 esp_restart() 会丢数据
       （小车模式退出/语音切换全中过这个坑）。重启前必须显式调它。 */
    void Commit();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
