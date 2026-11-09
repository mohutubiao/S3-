#include "wake_word_models.h"

#include "settings.h"

#define TAG "WakeWordCfg"

namespace wakeword {

namespace {

constexpr const char* kNamespace = "wakeword";
constexpr int kDefaultThreshold = 8;      /* 越小越敏感。2026-11-07 用户要求提升
                                             唤醒灵敏度，20 → 12；2026-11-08 用户
                                             要求再提 → 8（隔房间/背景音也能叫醒，
                                             代价：电视/音乐声可能偶发误唤醒，
                                             网页滑条 1~99 可随时回调）。 */
/* 自定义词的默认值不写死在这里：assets 的 index.json 里已经带了一份（由打包脚本写入），
   这里返回空串表示「没覆盖过，沿用镜像里的」。 */


std::vector<std::string> g_wakenets;
bool g_has_multinet = false;

/* 只收录实际会打包进 assets 的模型；其余模型名原样显示 */
const struct {
    const char* id;
    const char* name;
} kNames[] = {
    {"wn9_nihaoxiaozhi_tts",  "你好小智"},
    {"wn9_hiesp",             "嗨，乐鑫"},
    {"wn9_xiaoaitongxue",     "小爱同学"},
    {"wn9_alexa",             "Alexa"},
    {"wn9_sophia_tts",        "Sophia"},
    {"wn9_nihaomiaoban_tts2", "你好喵板"},
    {"wn9_hijason_tts2",      "Hi Jason"},
    {"wn9_himfive",           "Hi Five"},
};

}  // namespace

void RegisterAvailable(const std::vector<std::string>& wakenets, bool has_multinet) {
    g_wakenets = wakenets;
    g_has_multinet = has_multinet;
}

std::vector<std::string> AvailableWakenets() {
    return g_wakenets;
}

bool HasMultinet() {
    return g_has_multinet;
}

std::string GetMode() {
    Settings s(kNamespace, false);
    return s.GetString("mode", "");
}

void SetMode(const std::string& mode) {
    Settings s(kNamespace, true);
    s.SetString("mode", mode);
}

std::string GetCustomText() {
    Settings s(kNamespace, false);
    return s.GetString("text", "");
}

void SetCustomText(const std::string& text) {
    Settings s(kNamespace, true);
    s.SetString("text", text);
}

std::string GetCustomDisplay() {
    Settings s(kNamespace, false);
    return s.GetString("display", "");
}

void SetCustomDisplay(const std::string& display) {
    Settings s(kNamespace, true);
    s.SetString("display", display);
}

int GetThreshold() {
    Settings s(kNamespace, false);
    int v = s.GetInt("threshold", kDefaultThreshold);
    if (v < 1) {
        v = 1;
    } else if (v > 99) {
        v = 99;
    }
    return v;
}

void SetThreshold(int value) {
    if (value < 1) {
        value = 1;
    } else if (value > 99) {
        value = 99;
    }
    Settings s(kNamespace, true);
    s.SetInt("threshold", value);
}

bool IsValidCommand(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (unsigned char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == ' ' || c == '\'';
        if (!ok) {
            return false;   // 中文、全角标点等一律拒收
        }
    }
    return true;
}

std::string FactoryDefaultWakenet() {
#ifdef DEFAULT_WAKE_WORD_NAME
    return DEFAULT_WAKE_WORD_NAME;
#else
    return "";
#endif
}

std::string DisplayName(const std::string& model_name) {
    for (const auto& item : kNames) {
        if (model_name == item.id) {
            return item.name;
        }
    }
    return model_name;
}

}  // namespace wakeword
