#include "board_gestures.h"

#include <Arduino.h>

#include "serial_controller.h"

namespace {

/* 组合键判定：两键同时按住 40ms 才算组合，避免连按方向键时误触 */
constexpr uint32_t COMBO_SETTLE_MS = 40;
constexpr uint32_t COMBO_LONG_MS = 900;    /* 再按住 900ms -> B（返回） */
constexpr uint32_t PULSE_MS = 120;         /* 注入键的保持时长，够菜单去抖 */
constexpr uint32_t REPEAT_DELAY_MS = 500;  /* 方向键长按多久开始连发 */
constexpr uint32_t REPEAT_INTERVAL_MS = 140;

uint8_t pulseMask = 0;
uint32_t pulseUntilMs = 0;

bool comboActive = false;
bool comboLongFired = false;
uint32_t comboSinceMs = 0;

uint8_t repeatBit = 0;
uint32_t repeatStartMs = 0;
uint32_t repeatNextMs = 0;

void pulse(uint8_t mask, uint32_t now, uint32_t ms) {
    pulseMask = mask;
    pulseUntilMs = now + ms;
}

}  // namespace

BoardGestureResult boardGesturesUpdate(uint8_t rawMask) {
    BoardGestureResult out;
    const uint32_t now = millis();
    const bool up = (rawMask & DIJI_BTN_UP) != 0;
    const bool down = (rawMask & DIJI_BTN_DOWN) != 0;
    const bool both = up && down;

    /* ---- 方向键长按连发 ---- */
    const uint8_t dirBit = up ? DIJI_BTN_UP : (down ? DIJI_BTN_DOWN : 0);
    if (dirBit != 0 && !both) {
        if (repeatBit != dirBit) {
            repeatBit = dirBit;
            repeatStartMs = now;
            repeatNextMs = now + REPEAT_DELAY_MS;
        } else if ((uint32_t)(now - repeatStartMs) >= REPEAT_DELAY_MS &&
                   (int32_t)(now - repeatNextMs) >= 0) {
            pulse(dirBit, now, 60);
            repeatNextMs = now + REPEAT_INTERVAL_MS;
        }
    } else {
        repeatBit = 0;
    }

    /* ---- 上+下 组合键 ---- */
    if (both) {
        if (!comboActive) {
            comboActive = true;
            comboLongFired = false;
            comboSinceMs = now;
            pulse(DIJI_BTN_SELECT, now, PULSE_MS);
        } else if (!comboLongFired &&
                   (uint32_t)(now - comboSinceMs) >= COMBO_LONG_MS) {
            comboLongFired = true;
            pulse(DIJI_BTN_B, now, PULSE_MS);
        }
        /* 组合期间屏蔽方向，否则列表会一边滚一边切底排 */
        out.suppressed = DIJI_BTN_UP | DIJI_BTN_DOWN;
    } else {
        comboActive = false;
        comboLongFired = false;
    }

    if (pulseMask != 0) {
        if ((int32_t)(now - pulseUntilMs) < 0) {
            out.mask |= pulseMask;
        } else {
            pulseMask = 0;
        }
    }
    return out;
}

void boardGesturesReset() {
    pulseMask = 0;
    pulseUntilMs = 0;
    comboActive = false;
    comboLongFired = false;
    comboSinceMs = 0;
    repeatBit = 0;
    repeatStartMs = 0;
    repeatNextMs = 0;
}
