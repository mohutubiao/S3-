#ifndef WAKE_WORD_MODELS_H
#define WAKE_WORD_MODELS_H

#include <string>
#include <vector>

/* 唤醒词选择（2026-11-03）
   ---------------------------------
   模型本身来自 assets 分区的 srmodels.bin，**不在固件里** —— 所以：
     · 换唤醒词不需要重新烧录固件，只需要重新打包/烧写 assets 分区；
     · 但 AFE 初始化后不能再换模型（AudioService::SetModelsList 会拒绝），
       因此这里的选择是「存 NVS + 重启生效」。
   两个来源：
     1. 内置词（WakeNet）：如 wn9_nihaoxiaozhi_tts，可打包多个，网页里挑一个；
     2. 自定义词（MultiNet）：任意中文短语，如「小土豆」，运行时下发命令词。 */
namespace wakeword {

/* 引擎初始化时登记「这次镜子里有哪些模型」，供网页列表展示 */
void RegisterAvailable(const std::vector<std::string>& wakenets, bool has_multinet);
std::vector<std::string> AvailableWakenets();
bool HasMultinet();

/* 用户选择：内置模型名（如 "wn9_hiesp"）或 "custom"（走 MultiNet 自定义词）。
   空字符串 = 自动（有 Multinet 就用自定义词，否则用第一个内置词）。 */
std::string GetMode();
void SetMode(const std::string& mode);

/* 自定义唤醒词。esp_mn_commands_add 吃的是**拼音**（如 "xiao tu dou"），
   中文只用于显示/问候语，两者分开存。灵敏度 1-99，越小越敏感。 */
std::string GetCustomText();
void SetCustomText(const std::string& text);
std::string GetCustomDisplay();
void SetCustomDisplay(const std::string& display);
int GetThreshold();
void SetThreshold(int value);

/* 命令词合法性：只能是拼音（字母/空格/数字）。
   ⚠️ esp-sr 的 esp_mn_commands_add 传中文会空指针崩溃（实测
   phrase_init_from_phoneme_cn 里挂掉 → 开机无限重启），所以中文一律拦在门外，
   中文只作为显示文字存在 "display" 里。 */
bool IsValidCommand(const std::string& text);

/* 出厂默认内置词：sdkconfig 里选的那个（由 CMakeLists 传宏进来）。
   没设过 mode 时优先用它，避免"srmodels.bin 里字母序靠前的词"意外抢了默认。 */
std::string FactoryDefaultWakenet();

/* 模型名 -> 给人看的中文名；未收录的原样返回 */
std::string DisplayName(const std::string& model_name);

}  // namespace wakeword

#endif  // WAKE_WORD_MODELS_H
