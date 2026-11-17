/*******************************************************************************
 * Size: 14 px
 * Bpp: 1
 * Opts: --font C:\Users\mohut\xiaozhi-s3-board\_font_tools\NotoSansSC-Regular.ttf --size 14 --bpp 1 --format lvgl --lv-font-name font_ui_14_1 --no-compress -r 0x20-0x7F,38899,37327,20142,24230,20027,39064,26102,38047,22721,32440,25511,21046,39029,37197,32593,23567,36710,37325,21551,33756,21333,30830,35748,36873,25321,21462,28040,36820,22238,35774,32622,26041,21521,27178,31446,23631,35821,25171,26029,24320,20851,27169,24335,23383,24149,22825,27668,20986,36864,20303,25353 -o C:\Users\mohut\xiaozhi-s3-board\main\boards\zhengchen\1.54tft-dafeiyu\font_ui_14_1.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef FONT_UI_14_1
#define FONT_UI_14_1 1
#endif

#if FONT_UI_14_1

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0x20,

    /* U+0022 "\"" */
    0xb6, 0x80,

    /* U+0023 "#" */
    0x9, 0x24, 0xbf, 0x51, 0x49, 0x3f, 0x92, 0x49,
    0x0,

    /* U+0024 "$" */
    0x21, 0x1d, 0x8, 0x41, 0x4, 0x10, 0x42, 0x1f,
    0x10, 0x80,

    /* U+0025 "%" */
    0x70, 0x89, 0x10, 0x89, 0x8, 0xa0, 0x8a, 0x49,
    0x5a, 0x65, 0x10, 0x91, 0x9, 0x11, 0x12, 0x10,
    0xe0,

    /* U+0026 "&" */
    0x38, 0x22, 0x11, 0x9, 0x5, 0x1, 0x3, 0x45,
    0x22, 0x8a, 0x43, 0x1f, 0x60,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x9, 0x49, 0x24, 0x92, 0x44, 0x88,

    /* U+0029 ")" */
    0x11, 0x24, 0x49, 0x25, 0x25, 0x20,

    /* U+002A "*" */
    0x5d, 0x50,

    /* U+002B "+" */
    0x10, 0x20, 0x47, 0xf1, 0x2, 0x4, 0x0,

    /* U+002C "," */
    0x58,

    /* U+002D "-" */
    0xe0,

    /* U+002E "." */
    0x80,

    /* U+002F "/" */
    0x0, 0x20, 0x84, 0x10, 0x42, 0x8, 0x21, 0x4,
    0x10, 0x82, 0x0,

    /* U+0030 "0" */
    0x31, 0x28, 0xa1, 0x86, 0x18, 0x61, 0x89, 0x23,
    0x0,

    /* U+0031 "1" */
    0x61, 0x8, 0x42, 0x10, 0x84, 0x21, 0x1c,

    /* U+0032 "2" */
    0x7a, 0x20, 0x41, 0x4, 0x20, 0x84, 0x21, 0xf,
    0x80,

    /* U+0033 "3" */
    0x78, 0x10, 0x41, 0x8, 0xc0, 0x81, 0x6, 0x27,
    0x80,

    /* U+0034 "4" */
    0x8, 0x62, 0x8a, 0x49, 0x28, 0xbf, 0x8, 0x20,
    0x80,

    /* U+0035 "5" */
    0xf2, 0x8, 0x20, 0xb1, 0x20, 0x41, 0x4, 0x2f,
    0x0,

    /* U+0036 "6" */
    0x39, 0x8, 0x20, 0xb3, 0x28, 0x61, 0x85, 0x23,
    0x80,

    /* U+0037 "7" */
    0x7c, 0x20, 0x84, 0x10, 0x82, 0x8, 0x20, 0x82,
    0x0,

    /* U+0038 "8" */
    0x38, 0x89, 0x12, 0x22, 0x86, 0x33, 0x41, 0x83,
    0x9, 0xe0,

    /* U+0039 "9" */
    0x72, 0x28, 0xa1, 0x86, 0x37, 0x41, 0x8, 0x27,
    0x0,

    /* U+003A ":" */
    0x81,

    /* U+003B ";" */
    0x80, 0x1, 0x60,

    /* U+003C "<" */
    0xc, 0x63, 0x3, 0x1, 0x80, 0xc0,

    /* U+003D "=" */
    0xfc, 0x0, 0x3f,

    /* U+003E ">" */
    0xc0, 0x70, 0x18, 0x67, 0x10, 0x0,

    /* U+003F "?" */
    0x70, 0x42, 0x11, 0x10, 0x80, 0x0, 0x10,

    /* U+0040 "@" */
    0x7, 0x3, 0x11, 0x81, 0x23, 0xa8, 0x93, 0x24,
    0x64, 0x8c, 0x92, 0x92, 0x51, 0xb1, 0x0, 0x10,
    0x1, 0xf0,

    /* U+0041 "A" */
    0x10, 0x18, 0x28, 0x28, 0x24, 0x44, 0x3c, 0x42,
    0x42, 0x82, 0x81,

    /* U+0042 "B" */
    0xf9, 0xa, 0x14, 0x28, 0x9e, 0x23, 0x41, 0x83,
    0xb, 0xe0,

    /* U+0043 "C" */
    0x1c, 0xc6, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80,
    0x80, 0xf8,

    /* U+0044 "D" */
    0xf9, 0xa, 0x14, 0x18, 0x30, 0x60, 0xc1, 0x85,
    0xb, 0xe0,

    /* U+0045 "E" */
    0xfa, 0x8, 0x20, 0x83, 0xe8, 0x20, 0x82, 0xf,
    0x80,

    /* U+0046 "F" */
    0xfa, 0x8, 0x20, 0x83, 0xe8, 0x20, 0x82, 0x8,
    0x0,

    /* U+0047 "G" */
    0x1e, 0x60, 0x80, 0x80, 0x80, 0x87, 0x81, 0x81,
    0x81, 0x41, 0x3e,

    /* U+0048 "H" */
    0x83, 0x6, 0xc, 0x18, 0x3f, 0xe0, 0xc1, 0x83,
    0x6, 0x8,

    /* U+0049 "I" */
    0xff, 0xe0,

    /* U+004A "J" */
    0x8, 0x42, 0x10, 0x84, 0x21, 0xc, 0x5c,

    /* U+004B "K" */
    0x85, 0x12, 0x24, 0x8b, 0x1a, 0x22, 0x44, 0x85,
    0xa, 0x8,

    /* U+004C "L" */
    0x82, 0x8, 0x20, 0x82, 0x8, 0x20, 0x82, 0xf,
    0x80,

    /* U+004D "M" */
    0x81, 0xc3, 0xc3, 0xc5, 0xa5, 0xa5, 0xa9, 0xa9,
    0x99, 0x81, 0x81,

    /* U+004E "N" */
    0x83, 0x87, 0xd, 0x1a, 0x32, 0x64, 0xc5, 0x8b,
    0xe, 0x8,

    /* U+004F "O" */
    0x1c, 0x31, 0xa0, 0x50, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x81, 0x20, 0x8f, 0x80,

    /* U+0050 "P" */
    0xf9, 0xa, 0xc, 0x18, 0x30, 0xbc, 0x40, 0x81,
    0x2, 0x0,

    /* U+0051 "Q" */
    0x1c, 0x31, 0xa0, 0x50, 0x18, 0xc, 0x6, 0x3,
    0x1, 0x81, 0x20, 0x8d, 0x81, 0x0, 0x40, 0x1c,

    /* U+0052 "R" */
    0xfd, 0x6, 0xc, 0x18, 0x5f, 0x24, 0x44, 0x89,
    0xa, 0x8,

    /* U+0053 "S" */
    0x79, 0xa, 0x4, 0x4, 0x7, 0x1, 0x1, 0x3,
    0x9, 0xe0,

    /* U+0054 "T" */
    0x7c, 0x20, 0x40, 0x81, 0x2, 0x4, 0x8, 0x10,
    0x20, 0x40,

    /* U+0055 "U" */
    0x83, 0x6, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x9, 0xe0,

    /* U+0056 "V" */
    0x83, 0x6, 0xa, 0x24, 0x48, 0x8a, 0x14, 0x28,
    0x60, 0x40,

    /* U+0057 "W" */
    0x84, 0x30, 0xc5, 0x28, 0xa5, 0x14, 0xa4, 0x92,
    0x94, 0x51, 0x8a, 0x31, 0x46, 0x30, 0x42, 0x0,

    /* U+0058 "X" */
    0x82, 0x89, 0x21, 0x43, 0x2, 0xa, 0x14, 0x44,
    0x8a, 0x8,

    /* U+0059 "Y" */
    0x83, 0x9, 0x12, 0x42, 0x86, 0x4, 0x8, 0x10,
    0x20, 0x40,

    /* U+005A "Z" */
    0x3c, 0x8, 0x20, 0x41, 0x2, 0x8, 0x20, 0x41,
    0x3, 0xf0,

    /* U+005B "[" */
    0xd2, 0x49, 0x24, 0x92, 0x49, 0xc0,

    /* U+005C "\\" */
    0x84, 0x20, 0x84, 0x20, 0x84, 0x20, 0x84, 0x10,
    0x80,

    /* U+005D "]" */
    0x64, 0x92, 0x49, 0x24, 0x93, 0xc0,

    /* U+005E "^" */
    0x22, 0x95, 0x28, 0xc4,

    /* U+005F "_" */
    0xff,

    /* U+0060 "`" */
    0x88, 0x80,

    /* U+0061 "a" */
    0x78, 0x10, 0x47, 0x66, 0x18, 0x5f,

    /* U+0062 "b" */
    0x81, 0x2, 0x4, 0xb, 0x98, 0xa0, 0xc1, 0x83,
    0x6, 0x17, 0xc0,

    /* U+0063 "c" */
    0x39, 0x8, 0x20, 0x82, 0xc, 0xe,

    /* U+0064 "d" */
    0x2, 0x4, 0x8, 0x13, 0xe8, 0x60, 0xc1, 0x83,
    0x6, 0xb, 0xf0,

    /* U+0065 "e" */
    0x3b, 0x28, 0x7f, 0x82, 0x4, 0xe,

    /* U+0066 "f" */
    0x34, 0x44, 0x64, 0x44, 0x44, 0x44,

    /* U+0067 "g" */
    0x7f, 0xa, 0x14, 0x28, 0x9e, 0x20, 0x38, 0x87,
    0x6, 0x13, 0xc0,

    /* U+0068 "h" */
    0x82, 0x8, 0x20, 0xbb, 0x18, 0x61, 0x86, 0x18,
    0x61,

    /* U+0069 "i" */
    0x9f, 0xe0,

    /* U+006A "j" */
    0x20, 0x12, 0x49, 0x24, 0x92, 0x70,

    /* U+006B "k" */
    0x82, 0x8, 0x20, 0x8a, 0x4a, 0x28, 0xd2, 0x48,
    0xa2,

    /* U+006C "l" */
    0xff, 0xf0,

    /* U+006D "m" */
    0xb9, 0xd8, 0xc6, 0x10, 0xc2, 0x18, 0x43, 0x8,
    0x61, 0xc, 0x21,

    /* U+006E "n" */
    0xbb, 0x18, 0x61, 0x86, 0x18, 0x61,

    /* U+006F "o" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x51, 0x1c,

    /* U+0070 "p" */
    0xb9, 0x8a, 0xc, 0x18, 0x30, 0x61, 0x7c, 0x81,
    0x2, 0x4, 0x0,

    /* U+0071 "q" */
    0x3e, 0x86, 0xc, 0x18, 0x30, 0x60, 0xbf, 0x2,
    0x4, 0x8, 0x10,

    /* U+0072 "r" */
    0xac, 0x88, 0x88, 0x88,

    /* U+0073 "s" */
    0x74, 0x20, 0x83, 0x4, 0x3e,

    /* U+0074 "t" */
    0x44, 0x46, 0x44, 0x44, 0x44, 0x30,

    /* U+0075 "u" */
    0x86, 0x18, 0x61, 0x86, 0x18, 0x5f,

    /* U+0076 "v" */
    0x86, 0x14, 0x92, 0x48, 0xc3, 0x4,

    /* U+0077 "w" */
    0x84, 0x63, 0x14, 0xc9, 0x52, 0x52, 0x94, 0xa3,
    0x30, 0x84,

    /* U+0078 "x" */
    0x89, 0x25, 0xc, 0x31, 0x44, 0xa1,

    /* U+0079 "y" */
    0x86, 0x14, 0x52, 0x48, 0xa3, 0xc, 0x10, 0x82,
    0x30,

    /* U+007A "z" */
    0xf8, 0x84, 0x44, 0x22, 0x1e,

    /* U+007B "{" */
    0x29, 0x24, 0x94, 0x49, 0x24, 0x40,

    /* U+007C "|" */
    0xff, 0xff, 0x80,

    /* U+007D "}" */
    0x89, 0x24, 0x91, 0x49, 0x25, 0x0,

    /* U+007E "~" */
    0xc0, 0xe0,

    /* U+4E3B "主" */
    0x0, 0x0, 0x18, 0x0, 0x0, 0xff, 0xe0, 0x20,
    0x1, 0x0, 0x8, 0xf, 0xfe, 0x2, 0x0, 0x10,
    0x0, 0x80, 0x4, 0xf, 0xff, 0x80,

    /* U+4EAE "亮" */
    0x0, 0x0, 0x20, 0x7d, 0xe3, 0xfc, 0x40, 0x43,
    0xfc, 0x0, 0xf, 0xff, 0x8f, 0x11, 0x10, 0x11,
    0x2, 0x11, 0x40, 0xf0,

    /* U+4F4F "住" */
    0x1, 0x0, 0x84, 0x8, 0x0, 0x9f, 0xf6, 0x8,
    0x50, 0x40, 0x82, 0x5, 0xff, 0x20, 0x81, 0x4,
    0x8, 0x20, 0x41, 0x2, 0xff, 0x80,

    /* U+5173 "关" */
    0x0, 0x81, 0x8, 0x1, 0x7, 0xfe, 0x4, 0x0,
    0x40, 0x4, 0xf, 0xff, 0x6, 0x0, 0xa0, 0x9,
    0x1, 0x8, 0x60, 0x68, 0x0,

    /* U+51FA "出" */
    0x4, 0x0, 0x80, 0x10, 0xc2, 0x18, 0x43, 0x8,
    0x5f, 0xfc, 0x20, 0x84, 0x30, 0x86, 0x10, 0xff,
    0xf0, 0x0,

    /* U+5236 "制" */
    0x8, 0xa, 0x40, 0x52, 0x12, 0xfc, 0x98, 0x84,
    0xbf, 0xa4, 0x21, 0x2f, 0xe9, 0x49, 0x4a, 0x48,
    0x52, 0x42, 0x96, 0x10, 0x83, 0x80,

    /* U+5355 "单" */
    0x0, 0x0, 0x84, 0x2, 0x20, 0xff, 0xc4, 0x22,
    0x3f, 0xf1, 0x8, 0x8f, 0xfc, 0x2, 0x0, 0x10,
    0x3f, 0xfe, 0x4, 0x0, 0x20, 0x0,

    /* U+53D6 "取" */
    0xfc, 0x4, 0xbf, 0x4a, 0x17, 0x91, 0x49, 0x34,
    0x92, 0x78, 0xa4, 0x8c, 0x48, 0x47, 0x8c, 0x9,
    0x20, 0xa1,

    /* U+5411 "向" */
    0x4, 0x0, 0x80, 0x20, 0x7f, 0xf8, 0x3, 0x3f,
    0x64, 0x2c, 0x85, 0x90, 0xb3, 0xf6, 0x40, 0xc0,
    0x18, 0xe,

    /* U+542F "启" */
    0x2, 0x0, 0x0, 0x3f, 0xf2, 0x1, 0x20, 0x13,
    0xff, 0x20, 0x2, 0x0, 0x2f, 0xf7, 0x1, 0x50,
    0x14, 0xff, 0x80, 0x10,

    /* U+56DE "回" */
    0xff, 0xf8, 0x1, 0x80, 0x19, 0xf9, 0x90, 0x99,
    0x9, 0x90, 0x99, 0xf9, 0x80, 0x18, 0x1, 0xff,
    0xf8, 0x1,

    /* U+58C1 "壁" */
    0x0, 0x3, 0xef, 0xd1, 0x24, 0xf9, 0x4, 0x7,
    0x3e, 0x23, 0x13, 0xc7, 0x80, 0x0, 0x1, 0xff,
    0x80, 0x80, 0x4, 0x7, 0xff, 0x80,

    /* U+5929 "天" */
    0xff, 0xe0, 0x40, 0x4, 0x0, 0x40, 0x4, 0xf,
    0xff, 0xa, 0x0, 0xa0, 0x11, 0x2, 0x8, 0x40,
    0x40, 0x3,

    /* U+5B57 "字" */
    0x0, 0x0, 0x40, 0xff, 0xf8, 0x1, 0xbf, 0x90,
    0x8, 0x1, 0x0, 0x60, 0xff, 0xf0, 0x40, 0x4,
    0x0, 0x40, 0x1c, 0x0,

    /* U+5C0F "小" */
    0x2, 0x0, 0x10, 0x0, 0x80, 0x44, 0x2, 0x22,
    0x21, 0x9, 0x8, 0x50, 0x41, 0x82, 0x8, 0x10,
    0x0, 0x80, 0x4, 0x1, 0xe0, 0x0,

    /* U+5C4F "屏" */
    0x3f, 0xf8, 0x80, 0x23, 0xff, 0x89, 0x4, 0x22,
    0x20, 0xff, 0xf2, 0x22, 0x8, 0x88, 0x7f, 0xfd,
    0x10, 0x84, 0x42, 0x26, 0x8,

    /* U+5E55 "幕" */
    0x8, 0x83, 0xff, 0x82, 0x20, 0x7f, 0xc2, 0x2,
    0x1f, 0xf0, 0xff, 0x81, 0x0, 0x77, 0xf1, 0xff,
    0x18, 0x8e, 0x44, 0x42, 0x2e, 0x0,

    /* U+5EA6 "度" */
    0x0, 0x0, 0x10, 0x1f, 0xfe, 0x90, 0x87, 0xff,
    0xa4, 0x21, 0x21, 0x8, 0xf0, 0x7f, 0xe2, 0x1,
    0x22, 0x31, 0xe, 0x9, 0xce, 0x0,

    /* U+5F00 "开" */
    0x7f, 0xf8, 0x42, 0x2, 0x10, 0x10, 0x80, 0x84,
    0x7f, 0xfc, 0x21, 0x1, 0x8, 0x10, 0x40, 0x82,
    0x8, 0x10, 0x80, 0x80,

    /* U+5F0F "式" */
    0x1, 0x40, 0x12, 0x1, 0xf, 0xff, 0x1, 0x0,
    0x10, 0xff, 0x1, 0x10, 0x10, 0x81, 0x8, 0x12,
    0x83, 0xc5, 0x40, 0x30,

    /* U+6253 "打" */
    0x10, 0x0, 0x40, 0x1, 0x1f, 0x9f, 0x8, 0x10,
    0x20, 0x40, 0x81, 0x2, 0xe, 0x8, 0x50, 0x20,
    0x40, 0x81, 0x2, 0x4, 0x8, 0x70, 0xe0,

    /* U+62E9 "择" */
    0x20, 0x1, 0x3f, 0x88, 0x9, 0xf4, 0x82, 0x18,
    0x13, 0xa0, 0xc2, 0xcc, 0xfe, 0xa0, 0x81, 0x4,
    0xb, 0xfc, 0x41, 0xe, 0x8, 0x0,

    /* U+6309 "按" */
    0x20, 0x81, 0x0, 0x9, 0xff, 0xf9, 0x12, 0x50,
    0x90, 0x80, 0x9f, 0xe4, 0x40, 0x72, 0x21, 0xe,
    0x8, 0x10, 0x41, 0x6e, 0x70, 0x80,

    /* U+63A7 "控" */
    0x20, 0x1, 0x4, 0x9, 0xdf, 0xf8, 0x12, 0x2,
    0x11, 0x8, 0xc0, 0xc, 0xfe, 0xa0, 0x81, 0x4,
    0x8, 0x20, 0x41, 0xe, 0xff, 0x80,

    /* U+65AD "断" */
    0x10, 0x16, 0x8f, 0x25, 0x41, 0x22, 0xf, 0xd0,
    0x48, 0xfe, 0xc4, 0x9b, 0xa4, 0xd1, 0x24, 0x89,
    0x20, 0x89, 0xfc, 0x40, 0x22, 0x0,

    /* U+65B9 "方" */
    0x0, 0x0, 0x8, 0x0, 0x0, 0xff, 0xf0, 0x40,
    0x2, 0x0, 0x1f, 0xc1, 0x2, 0x8, 0x10, 0x40,
    0x84, 0x4, 0x40, 0x64, 0x1e, 0x0,

    /* U+65F6 "时" */
    0x0, 0x4f, 0x4, 0x90, 0x49, 0xff, 0x90, 0x4f,
    0x44, 0x94, 0x49, 0x24, 0x90, 0x49, 0x4, 0xf0,
    0x48, 0x4, 0x3, 0xc0,

    /* U+6A21 "模" */
    0x21, 0x21, 0x3f, 0xc8, 0x49, 0xf0, 0x2, 0x7f,
    0x19, 0xf9, 0xb0, 0x4c, 0x7e, 0xa0, 0x85, 0x7f,
    0xc8, 0x50, 0x44, 0x42, 0xc1, 0x80,

    /* U+6A2A "横" */
    0x10, 0x0, 0x89, 0x4, 0xfe, 0x20, 0x3, 0xff,
    0x88, 0x20, 0xcf, 0xeb, 0x49, 0x53, 0xfa, 0x92,
    0x64, 0xfe, 0x20, 0x1, 0x21, 0x80,

    /* U+6C14 "气" */
    0x0, 0x2, 0x0, 0x3f, 0xf4, 0x0, 0x9f, 0xc7,
    0xfc, 0x0, 0x40, 0x4, 0x0, 0x40, 0x4, 0x0,
    0x40, 0x3, 0x0, 0x10,

    /* U+6D88 "消" */
    0x40, 0x81, 0x24, 0x80, 0x20, 0x1, 0x8, 0x7f,
    0x22, 0x8, 0x10, 0x40, 0xfe, 0x4, 0x11, 0x3f,
    0x91, 0x4, 0x88, 0x28, 0x47, 0x0,

    /* U+786E "确" */
    0x0, 0xf, 0xbe, 0x42, 0x44, 0x44, 0x47, 0xf8,
    0x89, 0xb7, 0xf9, 0x89, 0x98, 0x99, 0xbf, 0x78,
    0x98, 0x89, 0x8, 0xb0,

    /* U+7AD6 "竖" */
    0x10, 0x1, 0x7e, 0x94, 0x49, 0x24, 0x91, 0x81,
    0x18, 0x16, 0x40, 0x2, 0x7f, 0xe0, 0x0, 0x10,
    0x80, 0x8, 0xff, 0xf0,

    /* U+7EB8 "纸" */
    0x0, 0x11, 0x1f, 0x8, 0x90, 0x84, 0x88, 0x24,
    0x39, 0x20, 0x8f, 0xe8, 0x48, 0x9a, 0x43, 0x12,
    0x2, 0x8a, 0xe7, 0x50, 0x21, 0x80,

    /* U+7F51 "网" */
    0xff, 0xf8, 0x1, 0x80, 0x1a, 0xa9, 0xa9, 0x99,
    0x9, 0x88, 0x99, 0x19, 0xa2, 0x5c, 0x61, 0x80,
    0x18, 0x7,

    /* U+7F6E "置" */
    0x7f, 0xf8, 0x91, 0x76, 0xef, 0xff, 0x4, 0x5,
    0xbc, 0x3f, 0xc4, 0x4, 0x5f, 0xc3, 0xfc, 0x40,
    0x4f, 0xff,

    /* U+83DC "菜" */
    0x8, 0x41, 0xff, 0xf0, 0x84, 0x0, 0xc, 0x7f,
    0xc0, 0x0, 0x2, 0x23, 0x0, 0x80, 0xff, 0xf8,
    0x2c, 0x1, 0x28, 0x8, 0x98, 0xc2, 0x18,

    /* U+8BA4 "认" */
    0x0, 0x40, 0xc1, 0x0, 0x84, 0x0, 0x10, 0x0,
    0x43, 0xc1, 0x1, 0x4, 0x4, 0x10, 0x10, 0xa0,
    0x42, 0x81, 0xa, 0x6, 0x44, 0x21, 0x8, 0x8,
    0x10,

    /* U+8BBE "设" */
    0x43, 0xc1, 0x12, 0x1, 0x10, 0x8, 0x6, 0x1,
    0x13, 0xf8, 0x80, 0x44, 0x44, 0x21, 0x41, 0x44,
    0xc, 0x50, 0xc, 0x70,

    /* U+8BED "语" */
    0x4f, 0xf0, 0x10, 0x1, 0xf8, 0x4, 0x46, 0x22,
    0x17, 0xfc, 0x80, 0x4, 0xfc, 0x28, 0x21, 0x41,
    0xe, 0x8, 0xf, 0xc0,

    /* U+8F66 "车" */
    0x8, 0x1, 0x0, 0xff, 0xf2, 0x0, 0x22, 0x4,
    0x20, 0x7f, 0xe0, 0x20, 0x2, 0xf, 0xff, 0x2,
    0x0, 0x20, 0x2, 0x0,

    /* U+8FD4 "返" */
    0x0, 0x1a, 0x3f, 0x9, 0x0, 0x8, 0x0, 0x7e,
    0x72, 0x10, 0x92, 0x85, 0x8, 0x29, 0xa1, 0x10,
    0x98, 0x0, 0xa0, 0x8, 0xff, 0x80,

    /* U+9000 "退" */
    0x47, 0xe1, 0x21, 0x1, 0xf8, 0x8, 0x46, 0x7e,
    0x12, 0x88, 0x93, 0x84, 0x88, 0x27, 0x23, 0x40,
    0x14, 0x1, 0x1f, 0xf0,

    /* U+9009 "选" */
    0x0, 0x82, 0x24, 0x9, 0xfe, 0x11, 0x0, 0x8,
    0x73, 0xb8, 0x89, 0x4, 0x48, 0x24, 0x49, 0x22,
    0x4a, 0xe, 0xe0, 0x9, 0xff, 0x80,

    /* U+914D "配" */
    0xfd, 0xf1, 0x80, 0xbf, 0x5, 0x68, 0x2b, 0x5f,
    0x5a, 0x8b, 0x34, 0x10, 0xa0, 0xfd, 0x4, 0x28,
    0x7f, 0x43, 0xb, 0xe0,

    /* U+91CD "重" */
    0x0, 0x33, 0xfe, 0x0, 0x80, 0xff, 0xe3, 0xff,
    0x11, 0x8, 0xff, 0xc4, 0x42, 0x3f, 0xf0, 0x10,
    0x1f, 0xfc, 0x4, 0xf, 0xff, 0x80,

    /* U+91CF "量" */
    0x3f, 0xc3, 0xfc, 0x3f, 0xc0, 0x0, 0x7f, 0xe7,
    0xfe, 0x44, 0x27, 0xfe, 0x7f, 0xe0, 0x40, 0x3f,
    0xcf, 0xff,

    /* U+949F "钟" */
    0x20, 0x42, 0x2, 0x1f, 0x10, 0x7, 0xf7, 0xe4,
    0x89, 0x24, 0x49, 0x2f, 0xc9, 0x13, 0xf8, 0x92,
    0x44, 0x10, 0x28, 0x82, 0x84, 0x0,

    /* U+97F3 "音" */
    0x0, 0x7, 0xfe, 0x0, 0x1, 0x8, 0x1, 0xf,
    0xff, 0x0, 0x3, 0xfc, 0x40, 0x43, 0xfc, 0x40,
    0x44, 0x4, 0x3f, 0xc0,

    /* U+9875 "页" */
    0xff, 0xf0, 0x40, 0x3f, 0xc4, 0x4, 0x40, 0x44,
    0x44, 0x44, 0x44, 0x44, 0x4, 0x0, 0x80, 0x10,
    0xce, 0x3,

    /* U+9898 "题" */
    0x7d, 0xfa, 0x22, 0x1f, 0x3c, 0xfa, 0x20, 0x15,
    0x7f, 0xa8, 0x45, 0x4b, 0xc8, 0x50, 0xa3, 0x88,
    0xa6, 0x1, 0xf, 0xf0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 49, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 62, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 84, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 5, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 14, .adv_w = 117, .box_w = 5, .box_h = 15, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 24, .adv_w = 197, .box_w = 12, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 41, .adv_w = 139, .box_w = 9, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 54, .adv_w = 52, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 55, .adv_w = 67, .box_w = 3, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 61, .adv_w = 67, .box_w = 3, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 67, .adv_w = 96, .box_w = 3, .box_h = 4, .ofs_x = 1, .ofs_y = 7},
    {.bitmap_index = 69, .adv_w = 117, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 76, .adv_w = 52, .box_w = 2, .box_h = 3, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 77, .adv_w = 73, .box_w = 3, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 78, .adv_w = 52, .box_w = 1, .box_h = 1, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 79, .adv_w = 89, .box_w = 6, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 90, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 99, .adv_w = 117, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 106, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 115, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 124, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 133, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 142, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 151, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 160, .adv_w = 117, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 170, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 179, .adv_w = 52, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 180, .adv_w = 52, .box_w = 2, .box_h = 10, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 183, .adv_w = 117, .box_w = 7, .box_h = 6, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 189, .adv_w = 117, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 192, .adv_w = 117, .box_w = 7, .box_h = 6, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 198, .adv_w = 97, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 205, .adv_w = 198, .box_w = 11, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 223, .adv_w = 129, .box_w = 8, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 234, .adv_w = 142, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 244, .adv_w = 139, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 254, .adv_w = 148, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 264, .adv_w = 126, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 273, .adv_w = 116, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 282, .adv_w = 148, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 156, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 303, .adv_w = 58, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 305, .adv_w = 113, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 312, .adv_w = 136, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 322, .adv_w = 114, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 331, .adv_w = 172, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 342, .adv_w = 156, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 352, .adv_w = 160, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 365, .adv_w = 134, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 375, .adv_w = 160, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 391, .adv_w = 132, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 401, .adv_w = 127, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 411, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 421, .adv_w = 155, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 431, .adv_w = 119, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 441, .adv_w = 189, .box_w = 11, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 457, .adv_w = 116, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 467, .adv_w = 108, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 477, .adv_w = 133, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 487, .adv_w = 67, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 493, .adv_w = 89, .box_w = 5, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 502, .adv_w = 67, .box_w = 3, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 508, .adv_w = 117, .box_w = 5, .box_h = 6, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 512, .adv_w = 123, .box_w = 8, .box_h = 1, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 513, .adv_w = 131, .box_w = 3, .box_h = 3, .ofs_x = 2, .ofs_y = 9},
    {.bitmap_index = 515, .adv_w = 120, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 521, .adv_w = 133, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 532, .adv_w = 110, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 538, .adv_w = 133, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 549, .adv_w = 118, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 555, .adv_w = 62, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 561, .adv_w = 119, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 572, .adv_w = 129, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 581, .adv_w = 55, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 583, .adv_w = 55, .box_w = 3, .box_h = 15, .ofs_x = -1, .ofs_y = -4},
    {.bitmap_index = 589, .adv_w = 112, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 598, .adv_w = 57, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 600, .adv_w = 199, .box_w = 11, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 611, .adv_w = 130, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 617, .adv_w = 131, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 624, .adv_w = 133, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 635, .adv_w = 133, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 646, .adv_w = 76, .box_w = 4, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 650, .adv_w = 99, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 655, .adv_w = 75, .box_w = 4, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 661, .adv_w = 129, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 667, .adv_w = 104, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 673, .adv_w = 166, .box_w = 10, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 683, .adv_w = 97, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 689, .adv_w = 105, .box_w = 6, .box_h = 12, .ofs_x = 0, .ofs_y = -4},
    {.bitmap_index = 698, .adv_w = 98, .box_w = 5, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 703, .adv_w = 67, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 709, .adv_w = 54, .box_w = 1, .box_h = 17, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 712, .adv_w = 67, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 718, .adv_w = 117, .box_w = 6, .box_h = 2, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 720, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 742, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 762, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 784, .adv_w = 224, .box_w = 12, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 805, .adv_w = 224, .box_w = 11, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 823, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 845, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 867, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 885, .adv_w = 224, .box_w = 11, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 903, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 923, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 941, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 963, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 981, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1001, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1023, .adv_w = 224, .box_w = 14, .box_h = 12, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1044, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1066, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1088, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1108, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1128, .adv_w = 224, .box_w = 14, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1151, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1173, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1195, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1217, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1239, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1261, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1281, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1303, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1325, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1345, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1367, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1387, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1407, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1429, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1447, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1465, .adv_w = 224, .box_w = 14, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1488, .adv_w = 224, .box_w = 14, .box_h = 14, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 1513, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1533, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1553, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1573, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1595, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1615, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1637, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1657, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1679, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1697, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 1719, .adv_w = 224, .box_w = 12, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1739, .adv_w = 224, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 1757, .adv_w = 224, .box_w = 13, .box_h = 12, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x73, 0x114, 0x338, 0x3bf, 0x3fb, 0x51a, 0x59b,
    0x5d6, 0x5f4, 0x8a3, 0xa86, 0xaee, 0xd1c, 0xdd4, 0xe14,
    0x101a, 0x106b, 0x10c5, 0x10d4, 0x1418, 0x14ae, 0x14ce, 0x156c,
    0x1772, 0x177e, 0x17bb, 0x1be6, 0x1bef, 0x1dd9, 0x1f4d, 0x2a33,
    0x2c9b, 0x307d, 0x3116, 0x3133, 0x35a1, 0x3d69, 0x3d83, 0x3db2,
    0x412b, 0x4199, 0x41c5, 0x41ce, 0x4312, 0x4392, 0x4394, 0x4664,
    0x49b8, 0x4a3a, 0x4a5d
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 20027, .range_length = 19038, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 51, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 0, 0, 1, 0, 0, 0, 0,
    1, 2, 0, 0, 0, 3, 4, 3,
    5, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 6, 6, 0, 0, 0,
    0, 0, 7, 8, 9, 10, 11, 12,
    13, 0, 0, 14, 15, 16, 0, 0,
    10, 17, 10, 18, 19, 20, 21, 22,
    23, 24, 25, 26, 2, 27, 0, 0,
    0, 0, 28, 29, 30, 0, 31, 32,
    33, 34, 0, 0, 35, 36, 34, 34,
    29, 29, 37, 38, 39, 40, 37, 41,
    42, 43, 44, 45, 2, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 0, 1, 2, 0, 0, 0, 0,
    2, 0, 3, 4, 0, 5, 6, 5,
    7, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 8, 8, 0, 0, 0,
    9, 0, 10, 0, 11, 0, 0, 0,
    11, 0, 0, 12, 0, 0, 0, 0,
    11, 0, 11, 0, 13, 14, 15, 16,
    17, 18, 19, 20, 0, 21, 3, 0,
    0, 0, 22, 0, 23, 23, 23, 24,
    25, 0, 26, 27, 0, 0, 28, 28,
    23, 28, 23, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 0, 0, 3, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    0, 0, 0, 0, -25, 0, 0, 0,
    0, -10, 0, -20, -2, 0, 0, 0,
    0, -2, 0, 0, 0, 0, -10, 0,
    0, 0, 0, 0, -5, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -5,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 20, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -17, 0, -28, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -24, -3, -14,
    -7, 0, -19, 0, 0, 0, -3, 0,
    0, 0, 4, 0, 0, -8, 0, -4,
    -3, 0, -5, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -5,
    -2, -10, 0, -4, -2, -5, -14, -5,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -7, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -10, -2, -20, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -5, -11, 0,
    -2, 10, 10, 0, 0, 5, -5, 0,
    0, 0, 0, 0, 0, 0, 0, -16,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -4, 0,
    0, 0, 0, 0, 0, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -10, 0, -25, 0, 0, 0, 0,
    -4, -2, -2, 0, 0, -10, -4, -2,
    0, 3, -2, -3, -10, 7, 0, -2,
    0, 0, 0, 0, 7, -2, -2, 0,
    0, 0, 0, 0, 0, 0, 0, -7,
    0, 0, 0, 0, 0, 0, 0, -3,
    -2, -5, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, 0, -2, -2, -2, -2, 0,
    0, 0, 0, 0, 0, -4, 0, 0,
    0, 0, -5, -2, -5, -5, -2, 0,
    0, 0, 0, -2, 0, 0, 0, 0,
    -5, 0, 0, 0, 0, -5, -2, -2,
    -2, 0, -2, 0, 0, 0, 0, -10,
    0, 0, -5, 0, 0, -2, 0, -7,
    0, -5, 0, -2, -2, -2, -5, -5,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, -5, 0, -2,
    0, -5, -2, 0, 0, 0, 0, 0,
    -14, 0, -21, 0, 0, -9, -2, -35,
    -3, 0, 0, 3, 3, -5, 3, -7,
    0, -7, -2, 0, -5, 0, 0, -5,
    -3, -2, -3, -2, -2, -4, -2, -3,
    0, 0, 0, -8, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -5, 0, -2,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -5, 0, 0, 0, 0, 0, 0, -10,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -3, 0, -10, 0, -3, 0, 0,
    0, -2, -3, 0, 0, -2, -2, -2,
    -2, 0, -2, 0, 0, 0, 0, 0,
    0, 0, -2, 0, 0, -5, -4, -2,
    -2, -2, -2, -2, 0, -12, 0, -34,
    0, -15, 0, 0, -4, 2, -4, 0,
    -4, -30, -5, -18, -13, 0, -17, 0,
    -20, 0, -2, -2, 0, 0, 0, 0,
    0, -3, -2, -6, -7, 0, -6, 0,
    0, 0, 0, 0, -24, -10, -24, 0,
    0, -12, 0, -37, -2, -8, 0, 0,
    0, -5, -2, -24, 0, -12, -6, 0,
    -10, 0, 0, 0, -2, 0, 0, 0,
    0, -2, 0, -5, 0, 0, 0, -2,
    0, -8, 0, 0, 0, 0, 0, -5,
    -3, -2, 0, 5, 2, 0, 2, -2,
    0, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -5, 0, 2, 0, 0,
    0, 0, 0, -2, -2, -5, 0, 0,
    0, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -5, 0, 0,
    0, 0, 0, 3, 0, 0, 0, 0,
    -24, -20, -26, -5, 0, -10, -5, -32,
    -12, 0, 0, 0, 0, -5, -2, -15,
    0, -20, -14, -5, -20, 0, 0, -12,
    -14, -5, -12, -10, -10, -12, -10, -21,
    0, 0, 0, 0, -2, 0, -11, 0,
    0, -5, 0, -10, -2, 0, 0, 0,
    0, -2, -2, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -14, -4, -15, 0, 0, -2, -2, -20,
    -2, 0, -2, 2, 0, 0, 0, -4,
    0, -3, -3, 0, -5, 0, 0, -5,
    -2, 0, -7, -2, -2, -3, -2, -7,
    0, 0, 0, 0, -7, -2, -11, 0,
    0, 0, -2, -19, -2, 0, 0, 0,
    0, 0, 0, -2, 0, -5, 0, 0,
    -5, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -2, 0, -2, 0, -2,
    0, -5, 0, 0, 0, 2, -2, 2,
    -2, -5, -2, 0, 0, 0, 0, 0,
    0, -2, 0, -2, 0, 0, 0, 0,
    0, -2, -2, -2, -2, -2, -2, -2,
    0, 0, 0, 0, -18, -14, -20, -4,
    0, -2, -2, -25, -3, -2, -2, 0,
    0, 0, 0, -7, 0, -15, -8, 0,
    -15, 0, 0, -10, -8, -3, -7, -2,
    -5, -7, -2, -10, 0, 0, 0, 0,
    0, -10, 0, 0, 0, -2, -5, -10,
    -7, 0, -2, -2, -2, 0, -2, -5,
    0, -5, -7, -5, -4, 0, 0, 0,
    0, -2, -7, -5, -5, -5, -5, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -24, -10, -15,
    -10, 0, -20, 0, 0, 0, 0, 0,
    10, 0, 20, 0, 0, 0, 0, -5,
    -2, 0, 5, 0, 0, 0, 0, -15,
    0, 0, 0, 0, 1, 0, 0, 0,
    0, -5, 0, -2, 0, 0, -5, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, -10, 0, -10, -2, 2, 0, 0,
    1, 0, 0, 0, 0, -14, 0, -4,
    0, 0, -9, 0, -4, -4, 0, 0,
    0, 0, 0, 0, 0, -2, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, -5, 0, -2, 0, 0, -5, 0,
    0, -2, -4, 0, -2, 0, 0, 0,
    0, -2, 0, 2, 2, 4, 2, 0,
    0, 0, 0, -10, 0, 2, 0, 0,
    0, 0, 0, -5, -5, -5, 0, -2,
    -2, 0, -4, 0, -2, -4, 0, 0,
    -2, 0, 0, 0, 0, -2, 0, 3,
    3, 0, 3, 2, 5, 10, 15, 0,
    -10, -2, -5, 0, 7, 0, 0, 0,
    0, 12, 0, 17, 12, 10, 17, 0,
    21, -5, -2, 0, -4, 0, -2, 0,
    0, 0, 0, 5, 0, 0, 0, -2,
    0, 0, 5, -10, 0, 0, 15, 0,
    -7, 0, 0, 0, 0, -4, 0, 0,
    0, 0, -2, 0, 0, -3, -2, 0,
    0, 0, 7, 0, 0, 0, 0, 0,
    0, 0, 6, -2, 0, 0, 0, -10,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -5, 0, -2, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -5, 5, -3, 0, 5,
    1, 0, 0, 0, 0, -5, 0, 0,
    0, 0, -2, 0, 0, -2, -2, 0,
    -2, 0, -2, 0, 0, -3, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -8, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -5, 0, -2,
    0, 0, -7, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -8, -5, -10, 10, 0, -2, 0, -10,
    0, 0, 0, 0, 0, 0, 0, -2,
    5, -5, -2, 0, -2, 0, 0, 0,
    0, 0, 0, 10, 7, 0, 10, 0,
    0, 0, 0, -16, 0, 2, 0, 0,
    0, 0, 0, 0, 0, -5, 0, -2,
    0, 0, -5, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -5, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    5, -4, 5, 5, -4, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, -5, -2, 0, -2, 0, 0, 0,
    -2, -5, 0, 0, 0, -5, 0, 0,
    0, 0, 0, -3, -4, 0, -5, 0,
    0, -2, 0, -10, 0, -5, 0, -2,
    0, 0, -2, -2, 0, -5, 0, 0,
    0, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, -5, 0, 0, 0, -3,
    -2, 0, -3, 0, 0, 0, 0, -7,
    0, -5, 0, -2, 0, -2, -5, 0,
    0, -2, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, -2,
    -2, 0, 0, -6, 0, -2, 0, 0,
    0, 0, -2, 0, 0, -10, 0, -3,
    0, -2, -7, 0, 0, -2, -2, 0,
    0, 0, 0, 0, 0, -5, 0, 0,
    0, 0, 2, 0, 0, 0, 0, 0,
    -4, 0, -5, 0, 0, 0, 0, -10,
    0, -5, 0, 0, 0, 0, 0, 0,
    0, -5, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, -2, 0, -5,
    0, 0, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, -7, 0, 0,
    0, 0, -10, 0, 0, -7, -2, 0,
    -2, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 45,
    .right_class_cnt     = 36,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_ui_14_1 = {
#else
lv_font_t font_ui_14_1 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 17,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
    .fallback = NULL,
    .user_data = NULL
};



#endif /*#if FONT_UI_14_1*/

