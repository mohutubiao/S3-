#pragma once

#include <stdint.h>

/* 板载 3 键手势层（2026-11-05）
   大肥鱼板只有 BOOT(A)=确认、音量+(上)、音量-(下)，没有 B / SELECT / 左右，
   也没有触摸 —— 菜单底部软键行原本根本进不去。这里用组合键补齐：

     上+下 同按          -> 脉冲一次 SELECT（点亮/熄灭底部软键行）
     上+下 同按并长按 1 秒 -> 脉冲一次 B（返回）
     单独长按 上 或 下     -> 按方向自动连发（长列表快速滚动）

   只在非游戏界面生效；打游戏时按键原样透传，不做任何加工。 */
struct BoardGestureResult {
    uint8_t mask = 0;        /* 需要注入的按键位（DIJI_BTN_*） */
    uint8_t suppressed = 0;  /* 需要屏蔽的物理键位（组合键期间不要滚动列表） */
};

BoardGestureResult boardGesturesUpdate(uint8_t rawMask);
void boardGesturesReset();
