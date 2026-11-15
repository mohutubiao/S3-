# 生成小智/游戏机开机「系统选择」列表菜单（320x240 RGB565）
# 2026-11-08 改列表式：两帧（高亮小智 / 高亮游戏盒子），固件里音量+/-切换、
# BOOT 确认、5 秒超时自动进当前项。
# 用法: python gen_bootmenu.py  （生成 4 个数组：小智侧 2 帧 + GameBox 侧 2 帧）
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
BG = (0x12, 0x14, 0x1C)
FG = (0xE8, 0xEC, 0xF5)
ACC = (0x3D, 0x5A, 0xFE)
GRAY = (0x8A, 0x93, 0xA8)
WHITE = (0xFF, 0xFF, 0xFF)

FONT_TITLE = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 34)
FONT_ITEM = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 30)
FONT_HINT = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttc", 18)

ITEMS = ["小智AI", "游戏盒子"]
ITEM_H = 56          # 列表行高
ITEM_W = 240         # 列表行宽
LIST_Y0 = 70         # 第一行顶部
HINT_Y = 208

def render_menu(selected):
    """selected: 0=小智AI  1=游戏盒子"""
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    t = "系统选择"
    tw = d.textlength(t, font=FONT_TITLE)
    d.text(((W - tw) / 2, 18), t, font=FONT_TITLE, fill=ACC)

    for i, name in enumerate(ITEMS):
        y = LIST_Y0 + i * (ITEM_H + 14)
        x0 = (W - ITEM_W) // 2
        if i == selected:
            # 选中项：ACC 圆角色块 + 白字 + 左侧白色指示条
            d.rounded_rectangle([x0, y, x0 + ITEM_W, y + ITEM_H], radius=12, fill=ACC)
            d.rectangle([x0 + 8, y + 12, x0 + 12, y + ITEM_H - 12], fill=WHITE)
            tw = d.textlength(name, font=FONT_ITEM)
            d.text(((W - tw) / 2, y + (ITEM_H - 30 * 1.35) / 2), name,
                   font=FONT_ITEM, fill=WHITE)
        else:
            d.rounded_rectangle([x0, y, x0 + ITEM_W, y + ITEM_H], radius=12,
                                outline=(0x30, 0x38, 0x50), width=2)
            tw = d.textlength(name, font=FONT_ITEM)
            d.text(((W - tw) / 2, y + (ITEM_H - 30 * 1.35) / 2), name,
                   font=FONT_ITEM, fill=FG)

    h = "音量+/- 选择    BOOT 确认"
    tw = d.textlength(h, font=FONT_HINT)
    d.text(((W - tw) / 2, HINT_Y), h, font=FONT_HINT, fill=GRAY)
    return img

def to_rgb565_bytes(img):
    data = bytearray()
    for px in img.getdata():
        r, g, b = px
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.append((v >> 8) & 0xFF)   # 大端：先发高字节
        data.append(v & 0xFF)
    return data

def to_rgb565_u16(img):
    out = []
    for px in img.getdata():
        r, g, b = px
        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out

def write_u8_header(path, names_sizes):
    with open(path, "w", encoding="utf-8") as f:
        f.write("// 自动生成（gen_bootmenu.py 2026-11-08 列表式）：系统选择菜单两帧\n")
        f.write("// 帧0=高亮小智AI  帧1=高亮游戏盒子  320x240 RGB565 大端\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        for name, data in names_sizes:
            f.write("static const uint8_t %s[%d] = {" % (name, len(data)))
            for i, b in enumerate(data):
                if i % 20 == 0:
                    f.write("\n")
                f.write("%d," % b)
            f.write("};\n\n")

def write_u16_header(path, names_sizes):
    with open(path, "w", encoding="utf-8") as f:
        f.write("// 自动生成（gen_bootmenu.py 2026-11-08 列表式）：与小智侧同款两帧菜单\n")
        f.write("// 帧0=高亮小智AI  帧1=高亮游戏盒子  320x240 RGB565\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("#include <pgmspace.h>\n")
        for name, u16 in names_sizes:
            f.write("static const uint16_t %s[%d] PROGMEM = {" % (name, len(u16)))
            for i, v in enumerate(u16):
                if i % 12 == 0:
                    f.write("\n")
                f.write("0x%04X," % v)
            f.write("};\n\n")

img0 = render_menu(0)
img1 = render_menu(1)

# 小智侧（esp_lcd draw_bitmap，uint8 大端字节流，两帧）
write_u8_header(
    r"C:\Users\mohut\xiaozhi-s3-board\main\boards\zhengchen\cam\dafeiyu_bootmenu.h",
    [("kBootMenuSel0", to_rgb565_bytes(img0)),
     ("kBootMenuSel1", to_rgb565_bytes(img1))])

# GameBox 侧（TFT_eSPI pushImage，uint16 RGB565，两帧）
write_u16_header(
    r"C:\Users\mohut\GameBox-NES-OpenSource\src\bootmenu_bitmap.h",
    [("kBootMenuSel0", to_rgb565_u16(img0)),
     ("kBootMenuSel1", to_rgb565_u16(img1))])

# 导出两张 PNG 方便肉眼检查版面
img0.save(r"C:\Users\mohut\xiaozhi-s3-board\main\boards\zhengchen\cam\_bootmenu_preview.png")
img1.save(r"C:\Users\mohut\GameBox-NES-OpenSource\_bootmenu_preview.png")
print("done: two frames generated")
