/*
 * scene.c — 天气时钟场景引擎
 * 从仿真器 weather-clock-preview.html 移植，三层合成：
 *   底色层 = 季节(4) × 昼夜(4态)
 *   天气层 = 8 类现象特效（与季节无关，叠加）
 *   天体层 = 日月沿弧线 + 8 档月相
 * 文字不在这里画，由 app_main 的 LVGL label 叠加。
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "wx_gfx.h"
#include "wx_types.h"
#include "wx_scene.h"

static const char *TAG = "scene";
static color_t *s_buf;
static int s_w, s_h;
static int s_moon_phase = MOON_FULL;  // 调试用：可用按键切换

/* ---------- 季节色板：白天/夜晚的天空、远山、近坡、地面 ---------- */
typedef struct {
    uint32_t day_top, day_bot, day_far, day_near, day_ground;
    uint32_t night_top, night_bot, night_far, night_near, night_ground;
} season_pal_t;

static const season_pal_t PAL[4] = {
    /* 春 */ {0x8FCFE8,0xEAF6F2,0xB7D8C0,0x7FB38C,0x8FBE96,
              0x16223E,0x33436A,0x273857,0x1B2A46,0x22314C},
    /* 夏 */ {0x4FBEE8,0xD6F2E6,0x8FD2B4,0x4FA87B,0x63B589,
              0x0C1732,0x263A63,0x1F3152,0x13233E,0x182A46},
    /* 秋 */ {0xF0B579,0xFCEACB,0xDDB08A,0xC08052,0xC89563,
              0x171D34,0x39344E,0x332E45,0x252137,0x2C273E},
    /* 冬 */ {0xAFC6DA,0xEFF4F7,0xCAD7E2,0x9FB2C2,0xE4EBF0,
              0x111A2C,0x2A3850,0x263448,0x1B2738,0x334155},
};

static void season_colors(season_t s, day_phase_t ph,
                          color_t *top,color_t *bot,color_t *far,
                          color_t *near,color_t *ground, bool *dark)
{
    const season_pal_t *p=&PAL[s];
    bool night = (ph==PHASE_NIGHT||ph==PHASE_DAWN||ph==PHASE_DUSK);
    *dark = night;
    if (ph==PHASE_DAY) {
        *top=gfx_hex(p->day_top);*bot=gfx_hex(p->day_bot);
        *far=gfx_hex(p->day_far);*near=gfx_hex(p->day_near);*ground=gfx_hex(p->day_ground);
    } else {
        color_t dt=gfx_hex(p->day_top),db=gfx_hex(p->day_bot);
        color_t nt=gfx_hex(p->night_top),nb=gfx_hex(p->night_bot);
        int k = (ph==PHASE_DAWN)?140:158;  // 黎明/黄昏混入夜色的比例
        *top=gfx_mix(dt,nt,k); *bot=gfx_mix(db,nb,k);
        *far=gfx_mix(gfx_hex(p->day_far),gfx_hex(p->night_far),k);
        *near=gfx_mix(gfx_hex(p->day_near),gfx_hex(p->night_near),k);
        *ground=gfx_mix(gfx_hex(p->day_ground),gfx_hex(p->night_ground),k);
        // 黎明橙 / 黄昏红橘 暖色覆盖
        color_t warm = (ph==PHASE_DAWN)?gfx_hex(0xF3A97A):gfx_hex(0xE8825A);
        *top=gfx_mix(*top,warm,76);
        *bot=gfx_mix(*bot,(ph==PHASE_DAWN)?gfx_hex(0xFBD9B0):gfx_hex(0xF4B184),92);
    }
}

/* 天气对天空的压暗/染色 */
static void tint_for_weather(wx_category_t w, color_t *top, color_t *bot, bool *dark)
{
    switch(w) {
        case WX_CLOUDY: *top=gfx_mix(*top,gfx_hex(0x9FB0BD),76); *bot=gfx_mix(*bot,gfx_hex(0xCBD6DC),56); break;
        case WX_RAIN: *top=gfx_mix(*top,gfx_hex(0x5E6E7C),158); *bot=gfx_mix(*bot,gfx_hex(0x8796A2),128); *dark=true; break;
        case WX_SNOW: *top=gfx_mix(*top,gfx_hex(0xAEBCC8),128); *bot=gfx_mix(*bot,gfx_hex(0xE4EBEF),90); break;
        case WX_THUNDER: *top=gfx_mix(*top,gfx_hex(0x3C4657),168); *bot=gfx_mix(*bot,gfx_hex(0x66707E),140); *dark=true; break;
        case WX_FOG: *top=gfx_mix(*top,gfx_hex(0xB9C4CB),140); *bot=gfx_mix(*bot,gfx_hex(0xDCE3E7),128); break;
        case WX_DUST: *top=gfx_mix(*top,gfx_hex(0xC9A268),140); *bot=gfx_mix(*bot,gfx_hex(0xE6CBA0),115); break;
        default: break;
    }
}

/* 三级贝塞尔山：用采样列高度填充 */
static void draw_hills(color_t far, color_t near, color_t ground)
{
    int top1[s_w], top2[s_w], top3[s_w];
    // 三次贝塞尔采样：给定 4 控制点，返回 t 处 y
    #define BEZ(p0,p1,p2,p3,t) ((int)(powf(1-(t),3)*(p0)+3*powf(1-(t),2)*(t)*(p1)+3*(1-(t))*(t)*(t)*(p2)+powf(t,3)*(p3)))
    for (int x=0;x<s_w;x++) {
        float u=(float)x/(s_w-1);
        // 第一层远山（波峰 86..116）
        if (u<0.28f){float t=u/0.28f;top1[x]=BEZ(116,96,104,86,t);}
        else if(u<0.53f){float t=(u-0.28f)/0.25f;top1[x]=BEZ(86,70,88,80,t);}
        else if(u<0.80f){float t=(u-0.53f)/0.27f;top1[x]=BEZ(80,70,92,82,t);}
        else {float t=(u-0.80f)/0.20f;top1[x]=BEZ(82,74,92,84,t);}
        // 第二层中景
        if (u<0.33f){float t=u/0.33f;top2[x]=BEZ(128,108,120,106,t);}
        else if(u<0.60f){float t=(u-0.33f)/0.27f;top2[x]=BEZ(106,94,116,104,t);}
        else if(u<0.90f){float t=(u-0.60f)/0.30f;top2[x]=BEZ(104,94,114,102,t);}
        else {float t=(u-0.90f)/0.10f;top2[x]=BEZ(102,96,110,106,t);}
        // 第三层近坡
        if (u<0.5f){float t=u/0.5f;top3[x]=BEZ(142,128,146,136,t);}
        else {float t=(u-0.5f)/0.5f;top3[x]=BEZ(136,126,144,134,t);}
    }
    color_t far_faded = gfx_mix(far, gfx_hex(0xFFFFFF), 40); // 空气透视：远山掺白/天空色
    gfx_fill_terrain(top1,152,far_faded);
    gfx_fill_terrain(top2,154,far);
    gfx_mist_band(134,158,gfx_hex(0xFFFFFF),46);           // 山间雾带
    gfx_fill_terrain(top3,168,near);
    gfx_fill_rect(0,168,s_w,s_h-168,ground);
    #undef BEZ
}

/* ---------- 天体：太阳/月亮沿弧线 ---------- */
static void arc_pos(float progress, int *x, int *y)
{
    *x = 24 + (int)(progress*192);
    *y = 96 - (int)(sinf(progress*3.14159265f)*66);
}

static void draw_sun(int x,int y,bool strong)
{
    gfx_radial_glow(x,y,32,gfx_hex(0xFFF6C8), strong?200:150);
    gfx_fill_circle(x,y,strong?12:9,gfx_hex(strong?0xFFE9A8:0xFFD98A));
}

static void draw_moon(int x,int y,int phase,color_t sky)
{
    const int r=11;
    gfx_fill_circle(x,y,r,gfx_hex(0xF3F5E9));
    // 用天空色遮挡圆做盈亏：dir=+1 盈(遮左露右)，-1 亏(遮右露左)
    if (phase!=4) {
        int o,dir;
        switch(phase){
            case 0:o=0;dir=1;break;   // 新月全遮
            case 1:o=3;dir=1;break;   // 蛾眉
            case 2:o=10;dir=1;break;  // 上弦 右半亮
            case 3:o=17;dir=1;break;  // 盈凸
            case 5:o=17;dir=-1;break; // 亏凸
            case 6:o=10;dir=-1;break; // 下弦 左半亮
            case 7:o=3;dir=-1;break;  // 残月
            default:o=3;dir=1;break;
        }
        int cx = x - dir*o;
        // 用背景天空色画遮挡圆（clip 在月盘内由半径相等近似）
        gfx_fill_circle(cx,y,r,sky);
    }
    gfx_ring(x,y,r,gfx_hex(0xDCE6FF),36);          // 暗面微光轮廓
    gfx_radial_glow(x,y,26,gfx_hex(0xF5F8FF),110); // 月晕
}

static void draw_stars(uint32_t tick)
{
    // 固定伪随机星点，亮度随 tick 闪烁
    for (int i=0;i<46;i++) {
        int sx=(i*73+17)%s_w, sy=(i*41+9)%110;
        int tw=(int)(180+75*sinf(tick/1000.0f*1.6f+i));
        color_t c=gfx_mix(0,gfx_hex(0xFFFFFF),tw>255?255:tw);
        gfx_pixel(sx,sy,c);
    }
}

/* ---------- 季节前景元素 ---------- */
static void draw_season_fg(season_t s, day_phase_t ph, uint32_t tick)
{
    bool night=(ph==PHASE_NIGHT||ph==PHASE_DAWN||ph==PHASE_DUSK);
    float t=tick/1000.0f;
    if (s==SEASON_SPRING) {
        // 柳枝 + 飘落花瓣
        for(int i=0;i<5;i++){int bx=8+i*13; gfx_line(bx-6,0,bx,58+i*3,night?gfx_hex(0x789C78):gfx_hex(0x568C5A));}
        for(int i=0;i<18;i++){int px=(i*47+(int)(t*12+i*20))%s_w; int py=(i*29+(int)(t*18))%s_h; gfx_pixel(px,py,gfx_hex(0xF7B6C8));}
    } else if (s==SEASON_SUMMER) {
        if (night) for(int i=0;i<20;i++){int fx=(i*53+11)%s_w,fy=140+(i*37)%60;int a=(int)(120+135*sinf(t*1.2f+i));gfx_pixel(fx,fy,gfx_mix(0,gfx_hex(0xE2FF8A),a>255?255:a));}
    } else if (s==SEASON_AUTUMN) {
        for(int i=0;i<20;i++){int lx=(i*59+(int)(t*10))%s_w;int ly=(i*31+(int)(t*22))%s_h;gfx_pixel(lx,ly,gfx_hex(i%2?0xD67A3A:0xE8A348));}
    } else if (s==SEASON_WINTER) {
        // 地面积雪：在 y=176 附近铺白
        gfx_fill_rect(0,176,s_w,16,gfx_hex(0xEEF3F6));
    }
}

/* ---------- 天气粒子层 ---------- */
static void draw_weather_fx(wx_category_t w, uint32_t tick)
{
    float t=tick/1000.0f;
    if (w==WX_RAIN || w==WX_THUNDER) {
        for(int i=0;i<70;i++){int rx=(i*37-(int)(t*40))%s_w;if(rx<0)rx+=s_w;int ry=(i*53+(int)(t*220+i*7))%s_h;gfx_line(rx,ry,rx-2,ry+10,gfx_hex(0xCEE0EC));}
        if (w==WX_THUNDER) {
            // 间歇闪白
            int flash=(int)(255*(0.5f+0.5f*sinf(t*1.1f)));
            if(flash>235){gfx_fill_rect_alpha(0,0,s_w,s_h,gfx_hex(0xFFFFFF),150);
                gfx_line(150,40,142,62,gfx_hex(0xFFFFE5));gfx_line(142,62,152,64,gfx_hex(0xFFFFE5));gfx_line(152,64,140,92,gfx_hex(0xFFFFE5));}
        }
    } else if (w==WX_SNOW) {
        for(int i=0;i<50;i++){int fx=(i*43+(int)(8*sinf(t+i)))%s_w;int fy=(i*29+(int)(t*30+i*5))%s_h;gfx_fill_circle(fx,fy,2,gfx_hex(0xFFFFFF));}
    } else if (w==WX_FOG) {
        for(int k=0;k<5;k++){int by=150+k*14;int bx=(k*60+(int)(t*20))%(s_w+80)-60;gfx_fill_rect_alpha(bx,by,180,10,gfx_hex(0xEBF0F3),46);}
    } else if (w==WX_DUST) {
        gfx_fill_rect_alpha(0,0,s_w,s_h,gfx_hex(0xBA9154),66);
        for(int i=0;i<50;i++){int dx=(i*31+(int)(t*120))%s_w;int dy=(i*47)%s_h;gfx_pixel(dx,dy,gfx_hex(0x96733C));}
    }
}

/* ---------- 对外：初始化与渲染 ---------- */
void scene_init(color_t *buf,int w,int h)
{ s_buf=buf; s_w=w; s_h=h; gfx_set_buffer(buf,w,h); }

void scene_render_bg(uint32_t tick_ms)
{
    if(!s_buf) return;
    time_t now=time(NULL);
    weather_data_t d; weather_get(&d);
    struct tm tm_now; localtime_r(&now,&tm_now);
    season_t season=weather_season(tm_now.tm_mon+1);
    day_phase_t phase=weather_day_phase((int64_t)now);

    color_t top,bot,far,near,ground; bool dark;
    season_colors(season,phase,&top,&bot,&far,&near,&ground,&dark);
    tint_for_weather(d.category,&top,&bot,&dark);

    // 天空三段渐变
    gfx_vgradient3(gfx_mix(top,bot,0),gfx_mix(top,bot,140),bot);

    // 地平线大气辉光
    uint32_t glow=0xFFE6A8; int ga=76;
    if(phase==PHASE_DAWN){glow=0xFF9E63;ga=102;}
    else if(phase==PHASE_DUSK){glow=0xFF7043;ga=112;}
    else if(phase==PHASE_NIGHT){glow=0x3E5480;ga=56;}
    gfx_radial_glow((int)(s_w*0.58f),150,150,gfx_hex(glow),ga);

    // 天体（晴/多云才出现；阴雨雪雾沙尘隐藏）
    bool celestial=(d.category==WX_CLEAR||d.category==WX_CLOUDY);
    if(celestial){
        int mx,my; arc_pos(weather_sun_progress(now),&mx,&my);
        if(phase==PHASE_NIGHT||phase==PHASE_DUSK){draw_stars(tick_ms);draw_moon(mx,my,d.moon,top);}
        else if(phase==PHASE_DAWN){draw_stars(tick_ms);draw_moon(mx,my,d.moon,top);draw_sun(mx+4,my,false);}
        else draw_sun(mx,my,true);
    } else if(phase==PHASE_NIGHT||phase==PHASE_DUSK) draw_stars(tick_ms);

    // 云（多云/阴/雨雪）
    if(d.category==WX_CLOUDY){gfx_fill_circle(90,58,16,gfx_hex(0xF4F7F9));gfx_fill_circle(120,54,20,gfx_hex(0xF4F7F9));}
    if(d.category==WX_RAIN||d.category==WX_SNOW||d.category==WX_THUNDER){gfx_fill_circle(100,56,22,gfx_hex(0x8E99A6));}

    // 山峦三层 + 雾
    draw_hills(far,near,ground);

    // 季节前景
    draw_season_fg(season,phase,tick_ms);

    // 天气粒子（最上层，不挡文字区）
    draw_weather_fx(d.category,tick_ms);

    // 底部信息条半透明底
    gfx_fill_rect_alpha(0,186,s_w,s_h-186,dark?gfx_hex(0x0A121E):gfx_hex(0xFFFFFF),dark?110:120);
}

/* ---------- 文字层数据组装 ---------- */
void scene_build_text(scene_text_t *out)
{
    if(!out) return;
    time_t now=time(NULL);
    struct tm *tm=localtime(&now);
    weather_data_t d; weather_get(&d);
    day_phase_t ph=weather_day_phase((int64_t)now);
    season_t season=weather_season(tm->tm_mon+1);

    snprintf(out->time_hm,sizeof(out->time_hm),"%02d:%02d",tm->tm_hour,tm->tm_min);
    const char *wk[]={"周日","周一","周二","周三","周四","周五","周六"};
    snprintf(out->date,sizeof(out->date),"%d月%d日 %s",tm->tm_mon+1,tm->tm_mday,wk[tm->tm_wday]);
    snprintf(out->temp,sizeof(out->temp),"%d°",(int)d.temp);
    snprintf(out->feels,sizeof(out->feels),"体感%d°",(int)d.feels_like);
    snprintf(out->desc,sizeof(out->desc),"%s",d.text[0]?d.text:"--");
    snprintf(out->range,sizeof(out->range),"%d° ~ %d°",(int)d.temp_min,(int)d.temp_max);
    /* out->aqi 只有 16 字节，"AQI " + 15 字中文会溢出（-Werror=format-truncation） */
    snprintf(out->aqi,sizeof(out->aqi),"AQI %.8s",d.aqi_category[0]?d.aqi_category:"--");
    const char *sn[]={"春","夏","秋","冬"};
    out->season=sn[season];
    out->dark_text = (ph!=PHASE_DAY) || d.category==WX_RAIN || d.category==WX_THUNDER;
}
