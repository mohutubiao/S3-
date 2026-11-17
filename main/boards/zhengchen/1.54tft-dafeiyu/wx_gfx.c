/*
 * gfx.c — RGB565 帧缓冲绘制实现
 * 全部整数运算，适合 MCU
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "wx_gfx.h"

static color_t *s_buf = NULL;
static int s_w = 0, s_h = 0;

void gfx_set_buffer(color_t *buf, int w, int h)
{ s_buf = buf; s_w = w; s_h = h; }

color_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{ return (color_t)(((r&0xF8)<<11)|((g&0xFC)<<5)|(b&0xF8)); }

color_t gfx_hex(uint32_t v)
{ return gfx_rgb((v>>16)&0xFF,(v>>8)&0xFF,v&0xFF); }

color_t gfx_mix(color_t a, color_t b, int t)
{
    if (t<0) t=0;
    if (t>255) t=255;
    int ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
    int br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
    int r=ar+((br-ar)*t>>8), g=ag+((bg-ag)*t>>8), bl=ab+((bb-ab)*t>>8);
    return (color_t)((r<<11)|(g<<5)|bl);
}

color_t gfx_alpha(color_t c, int a)
{
    if (a<0) a=0;
    if (a>255) a=255;
    return gfx_mix(0, c, a);   // 与黑混合实现压暗
}

void gfx_clear(color_t c)
{ if (s_buf) for (int i=0;i<s_w*s_h;i++) s_buf[i]=c; }

void gfx_pixel(int x, int y, color_t c)
{
    if (!s_buf || x<0||y<0||x>=s_w||y>=s_h) return;
    s_buf[y*s_w+x]=c;
}

void gfx_fill_rect(int x,int y,int w,int h,color_t c)
{
    if (!s_buf) return;
    if (x<0){w+=x;x=0;}
    if (y<0){h+=y;y=0;}
    if (x+w>s_w) w=s_w-x;
    if (y+h>s_h) h=s_h-y;
    if (w<=0||h<=0) return;
    for (int row=0; row<h; row++)
        memset(&s_buf[(y+row)*s_w+x], c, w*sizeof(color_t));
}

void gfx_fill_rect_alpha(int x,int y,int w,int h,color_t c,int a)
{
    if (!s_buf) return;
    if (x<0){w+=x;x=0;}
    if (y<0){h+=y;y=0;}
    if (x+w>s_w) w=s_w-x;
    if (y+h>s_h) h=s_h-y;
    if (w<=0||h<=0) return;
    for (int row=0; row<h; row++) {
        color_t *p=&s_buf[(y+row)*s_w+x];
        for (int col=0; col<w; col++) p[col]=gfx_mix(p[col],c,a);
    }
}

void gfx_vgradient(int x,int y,int w,int h,color_t top,color_t bot)
{
    if (!s_buf) return;
    for (int row=0; row<h; row++) {
        int t = (h<=1)?0:(row*255/(h-1));
        color_t c = gfx_mix(top,bot,t);
        color_t *p=&s_buf[(y+row)*s_w+x];
        for (int col=0; col<w; col++) p[col]=c;
    }
}

void gfx_vgradient3(color_t c0, color_t c1, color_t c2)
{
    if (!s_buf) return;
    int half=s_h/2;
    for (int y=0; y<s_h; y++) {
        color_t c = (y<half)? gfx_mix(c0,c1,y*255/(half>0?half:1))
                            : gfx_mix(c1,c2,(y-half)*255/((s_h-half)>0?(s_h-half):1));
        color_t *p=&s_buf[y*s_w];
        for (int x=0;x<s_w;x++) p[x]=c;
    }
}

void gfx_fill_circle(int cx,int cy,int r,color_t c)
{
    if (!s_buf) return;
    int r2=r*r;
    for (int y=-r;y<=r;y++) {
        int dx2 = r2 - y*y;
        if (dx2<0) continue;
        int dx = 0; while ((dx+1)*(dx+1)<=dx2) dx++;
        int yy=cy+y;
        if (yy<0||yy>=s_h) continue;
        int x0=cx-dx, x1=cx+dx;
        if (x0<0)x0=0;
        if(x1>=s_w)x1=s_w-1;
        if (x0>x1) continue;
        memset(&s_buf[yy*s_w+x0], c, (x1-x0+1)*sizeof(color_t));
    }
}

void gfx_ring(int cx,int cy,int r,color_t c,int a)
{
    if (!s_buf) return;
    for (int deg=0; deg<360; deg++) {
        float rad=deg*3.14159265f/180;
        int x=cx+(int)(r*cosf(rad)), y=cy+(int)(r*sinf(rad));
        if (x>=0&&y>=0&&x<s_w&&y<s_h) s_buf[y*s_w+x]=gfx_mix(s_buf[y*s_w+x],c,a);
    }
}

void gfx_line(int x0,int y0,int x1,int y1,color_t c)
{
    int dx=abs(x1-x0), dy=-abs(y1-y0);
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx+dy;
    while (1) {
        gfx_pixel(x0,y0,c);
        if (x0==x1&&y0==y1) break;
        int e2=2*err;
        if (e2>=dy){err+=dy;x0+=sx;}
        if (e2<=dx){err+=dx;y0+=sy;}
    }
}

void gfx_hline(int x0,int x1,int y,color_t c,int a)
{
    if (y<0||y>=s_h) return;
    if (x0<0)x0=0;
    if(x1>=s_w)x1=s_w-1;
    for (int x=x0;x<=x1;x++) s_buf[y*s_w+x]=gfx_mix(s_buf[y*s_w+x],c,a);
}

void gfx_radial_glow(int cx,int cy,int radius,color_t c,int strength)
{
    if (!s_buf||radius<=0) return;
    int r2=radius*radius;
    for (int y=-radius;y<=radius;y++) {
        int yy=cy+y; if(yy<0||yy>=s_h) continue;
        for (int x=-radius;x<=radius;x++) {
            int xx=cx+x; if(xx<0||xx>=s_w) continue;
            int d2=x*x+y*y; if(d2>r2) continue;
            int a = strength * (r2-d2) / r2;   // 中心强边缘弱
            int idx=yy*s_w+xx;
            s_buf[idx]=gfx_mix(s_buf[idx],c,a);
        }
    }
}

void gfx_fill_terrain(const int *top_y,int bottom_y,color_t c)
{
    if (!s_buf) return;
    for (int x=0;x<s_w;x++) {
        int t=top_y[x]; if(t<0)t=0; if(t>bottom_y)t=bottom_y;
        for (int y=t;y<bottom_y;y++) s_buf[y*s_w+x]=c;
    }
}

void gfx_mist_band(int y0,int y1,color_t c,int strength)
{
    if (!s_buf) return;
    int h=y1-y0; if(h<=0) return;
    for (int y=y0;y<y1;y++) {
        // 中间浓上下淡
        int d = (y<y0+h/2)? (y-y0) : (y1-y-1);
        int a = strength * d / (h/2>0?h/2:1);
        for (int x=0;x<s_w;x++) {
            int idx=y*s_w+x;
            s_buf[idx]=gfx_mix(s_buf[idx],c,a);
        }
    }
}
