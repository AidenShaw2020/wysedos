/* SPDX-License-Identifier: GPL-3.0-or-later
 * Guest-only DirectDraw test. Proves hardware submission AND exact pixels,
 * including dirty CPU cache lines adjacent to a later GPU fill rectangle.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include "display/wgpu_display.h"
static HANDLE output;
void *memset(void *p,int value,size_t count){unsigned char *b=p;while(count--)*b++=(unsigned char)value;return p;}
void *memcpy(void *p,const void *s,size_t count){unsigned char *d=p;const unsigned char *b=s;while(count--)*d++=*b++;return p;}
static void say(const char *s){DWORD n;WriteFile(output,s,lstrlenA(s),&n,NULL);}
static void hex(const char *s,DWORD value)
{
    char b[11];static const char digits[]="0123456789abcdef";
    unsigned i;say(s);for(i=0;i<8;++i)b[i]=digits[(value>>(28-i*4))&15];
    b[8]='\r';b[9]='\n';b[10]=0;say(b);
}
static int query(WgpuNtInfo *info)
{
    struct {WgpuDisplayReply header;WgpuNtInfo info;} reply;
    WgpuDisplayRequest request={sizeof(request),WGPU_DISPLAY_ABI,WGPU_DISPLAY_QUERY,0};
    HDC dc=CreateDCA("DISPLAY",NULL,NULL,NULL);
    int result;
    if(!dc)return 0;
    memset(&reply,0,sizeof(reply));
    result=ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(request),(LPCSTR)&request,sizeof(reply),(LPSTR)&reply);
    DeleteDC(dc);
    if(result<=0 || reply.header.size!=sizeof(reply) || reply.header.abi_version!=WGPU_DISPLAY_ABI ||
       reply.header.status || reply.header.bytes!=sizeof(*info) ||
       reply.info.size!=sizeof(*info) || reply.info.abi_version!=WGPU_NT_ABI_VERSION ||
       reply.info.magic!=WGPU_NT_MAGIC || (reply.info.status&3)!=1)return 0;
    *info=reply.info;return 1;
}
static DWORD pixel(void *memory,LONG pitch,unsigned bpp,unsigned x,unsigned y)
{
    BYTE *p=(BYTE *)memory+y*pitch+x*(bpp/8);
    return bpp==16?*(WORD *)p:*(DWORD *)p;
}
static void put_pixel(void *memory,LONG pitch,unsigned bpp,unsigned x,unsigned y,DWORD color)
{
    BYTE *p=(BYTE *)memory+y*pitch+x*(bpp/8);
    if(bpp==16)*(WORD *)p=(WORD)color;else *(DWORD *)p=color;
}
static int check_pixels(LPDIRECTDRAWSURFACE surface,int partial,unsigned expected_bpp)
{
    DDSURFACEDESC locked;
    HRESULT hr;
    unsigned x,y,bpp;
    DWORD background,foreground,cpu;
    int pass=1;
    memset(&locked,0,sizeof(locked));locked.dwSize=sizeof(locked);
    hr=IDirectDrawSurface_Lock(surface,NULL,&locked,DDLOCK_WAIT|DDLOCK_READONLY,NULL);
    if(FAILED(hr)){hex("FAIL Lock=",hr);return 0;}
    bpp=locked.ddpfPixelFormat.dwRGBBitCount;
    background=bpp==16?0x5aed:0x7f12abef;
    foreground=bpp==16?0xe367:0x2567c319;
    cpu=bpp==16?0x39a7:0xa562b438;
    if(bpp!=expected_bpp || !locked.lpSurface || locked.lPitch<=0 ||
       locked.dwWidth!=256 || locked.dwHeight!=128 ||
       !(locked.ddsCaps.dwCaps&DDSCAPS_VIDEOMEMORY))pass=0;
    for(y=0;pass&&y<128;++y)for(x=0;x<256;++x){
        DWORD wanted=partial&&x>=7&&x<253&&y>=3&&y<119?foreground:background;
        if(partial&&((x==6&&y==3)||(x==253&&y==118)))wanted=cpu;
        if(pixel(locked.lpSurface,locked.lPitch,bpp,x,y)!=wanted){
            hex("pixel_x=",x);hex("pixel_y=",y);hex("expected=",wanted);
            hex("actual=",pixel(locked.lpSurface,locked.lPitch,bpp,x,y));pass=0;break;
        }
    }
    hr=IDirectDrawSurface_Unlock(surface,NULL);
    if(FAILED(hr)){hex("FAIL Unlock=",hr);pass=0;}
    return pass;
}
static int dirty_edges(LPDIRECTDRAWSURFACE surface,unsigned bpp)
{
    DDSURFACEDESC locked;
    HRESULT hr;
    DWORD color=bpp==16?0x39a7:0xa562b438;
    memset(&locked,0,sizeof(locked));locked.dwSize=sizeof(locked);
    hr=IDirectDrawSurface_Lock(surface,NULL,&locked,DDLOCK_WAIT,NULL);
    if(FAILED(hr))return 0;
    put_pixel(locked.lpSurface,locked.lPitch,bpp,6,3,color);
    put_pixel(locked.lpSurface,locked.lPitch,bpp,253,118,color);
    return SUCCEEDED(IDirectDrawSurface_Unlock(surface,NULL));
}

static DWORD copy_pattern(unsigned x, unsigned y, unsigned bpp)
{
    DWORD value=0x8eb967a3u*x+0x9c654f29u*y+0x237ea15bu;
    return bpp==16 ? value&0xffff : value;
}

static int copy_pixels(LPDIRECTDRAWSURFACE surface, unsigned width, unsigned bpp,
                       int destination, int verify)
{
    DDSURFACEDESC locked;
    HRESULT hr;
    int pass=1;
    memset(&locked,0,sizeof(locked));
    locked.dwSize=sizeof(locked);
    hr=IDirectDrawSurface_Lock(surface,NULL,&locked,DDLOCK_WAIT,NULL);
    if (FAILED(hr)) {hex("FAIL copy Lock=",hr);return 0;}
    if (!locked.lpSurface || locked.lPitch<(LONG)(width*(bpp/8)) ||
        locked.dwWidth!=width || locked.dwHeight!=128 ||
        locked.ddpfPixelFormat.dwRGBBitCount!=bpp) pass=0;
    for (unsigned y=0;pass && y<128;++y) for (unsigned x=0;x<width;++x) {
        DWORD wanted=destination ? copy_pattern(width-1-x,127-y,bpp) : copy_pattern(x,y,bpp);
        if (destination && verify && x>=7 && x<249 && y>=3 && y<119)
            wanted=copy_pattern(x-4,y+2,bpp);
        if (!verify) put_pixel(locked.lpSurface,locked.lPitch,bpp,x,y,wanted);
        else if (pixel(locked.lpSurface,locked.lPitch,bpp,x,y)!=wanted) {
            hex("pixel_x=",x);hex("pixel_y=",y);hex("expected=",wanted);
            hex("actual=",pixel(locked.lpSurface,locked.lPitch,bpp,x,y));
            pass=0;
            break;
        }
    }
    hr=IDirectDrawSurface_Unlock(surface,NULL);
    return pass && SUCCEEDED(hr);
}

static int copy_test(LPDIRECTDRAWSURFACE src, LPDIRECTDRAWSURFACE dst, unsigned width, unsigned bpp)
{
    RECT source={3,5,245,121}, dest={7,3,249,119};
    WgpuNtInfo before,after;
    HRESULT hr;
    if (!copy_pixels(src,width,bpp,0,0) || !copy_pixels(dst,width,bpp,1,0) || !query(&before)) {
        say("FAIL preparing copy\r\n");return 0;
    }
    hr=IDirectDrawSurface_Blt(dst,&dest,src,&source,DDBLT_WAIT,NULL);
    if (FAILED(hr)) {hex("FAIL copy Blt=",hr);return 0;}
    if (!copy_pixels(dst,width,bpp,1,1) || !copy_pixels(src,width,bpp,0,1) || !query(&after)) {
        say("FAIL copy pixels or GPU query\r\n");return 0;
    }
    hex("submitted_delta=",after.submitted-before.submitted);
    if (after.submitted-before.submitted!=1 || after.completed!=after.submitted ||
        after.user_3d_submits!=before.user_3d_submits || after.resets!=before.resets) {
        HDC dc=CreateDCA("DISPLAY",NULL,NULL,NULL);
        WgpuCopyTrace trace;
        if (dc && ExtEscape(dc,WGPU_DISPLAY_COPY_TRACE,0,NULL,sizeof(trace),(LPSTR)&trace)>0) {
            hex("stage=",trace.stage);hex("flags=",trace.flags);
            hex("rop=",trace.rop);hex("clipped=",trace.clipped);
            if (trace.stage>=3) {
                hex("src_offset=",trace.src_offset);hex("src_pitch=",trace.src_pitch);
                hex("dst_offset=",trace.dst_offset);hex("dst_pitch=",trace.dst_pitch);
            }
        }
        if (dc) DeleteDC(dc);
        say("FAIL: DirectDraw did not complete exactly one hardware copy\r\n");return 0;
    }
    hex("tested_bpp=",bpp);
    say("PASS: GPU copy, exact pixels and no application 3D tag\r\n");
    return 1;
}
void __attribute__((stdcall,noreturn)) mainCRTStartup(void)
{
    LPDIRECTDRAW dd=NULL;
    LPDIRECTDRAWSURFACE surface=NULL, source=NULL;
    HWND window=NULL;
    DDSURFACEDESC description,mode;
    DDBLTFX fx;
    WgpuNtInfo before,after;
    RECT rectangle={7,3,253,119};
    HRESULT hr;
    DWORD result=1;
    unsigned bpp=32,iteration;
    int fullscreen=0, copy=0;
    char *command=GetCommandLineA();
    output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(*command=='"'){++command;while(*command&&*command!='"')++command;if(*command)++command;}
    else while(*command&&*command!=' ')++command;
    while(*command==' ')++command;
    if (*command) {
        if (!lstrcmpA(command,"--16")) fullscreen=1;
        else if (!lstrcmpA(command,"--copy")) copy=1;
        else if (!lstrcmpA(command,"--copy16")) copy=fullscreen=1;
        else {say("Usage: NTDFILL [--16|--copy|--copy16]\r\n");goto done;}
    }
    hr=DirectDrawCreate(NULL,&dd,NULL);
    if(FAILED(hr)){hex("FAIL DirectDrawCreate=",hr);goto done;}
    window=CreateWindowExA(0,"STATIC","NT DirectDraw GPU test",WS_POPUP,
                          0,0,640,480,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!window){say("FAIL test window\r\n");goto done;}
    if(fullscreen){ShowWindow(window,SW_SHOW);SetForegroundWindow(window);}
    hr=IDirectDraw_SetCooperativeLevel(dd,window,fullscreen?DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN:DDSCL_NORMAL);
    if(FAILED(hr)){hex("FAIL cooperative level=",hr);goto done;}
    if(fullscreen){
        hr=IDirectDraw_SetDisplayMode(dd,640,480,16);
        if(FAILED(hr)){hex("FAIL display mode=",hr);goto done;}
        bpp=16;
    }
    memset(&mode,0,sizeof(mode));mode.dwSize=sizeof(mode);
    hr=IDirectDraw_GetDisplayMode(dd,&mode);
    if(FAILED(hr)||mode.ddpfPixelFormat.dwRGBBitCount!=bpp){say("FAIL expected RGB display mode\r\n");goto done;}
    memset(&description,0,sizeof(description));description.dwSize=sizeof(description);
    description.dwFlags=DDSD_CAPS|DDSD_WIDTH|DDSD_HEIGHT;
    /* Request full-width rows so the rectangular VRAM heap cannot put the
     * copy test's allocations side by side on the same physical rows. */
    description.dwWidth=copy?mode.dwWidth:256;description.dwHeight=128;
    description.ddsCaps.dwCaps=DDSCAPS_OFFSCREENPLAIN|DDSCAPS_VIDEOMEMORY;
    hr=IDirectDraw_CreateSurface(dd,&description,&surface,NULL);
    if(FAILED(hr)){hex("FAIL video surface=",hr);goto done;}
    if (copy) {
        hr=IDirectDraw_CreateSurface(dd,&description,&source,NULL);
        if (FAILED(hr)) {hex("FAIL source surface=",hr);goto done;}
        result=copy_test(source,surface,mode.dwWidth,bpp)?0:1;
        goto done;
    }
    memset(&fx,0,sizeof(fx));fx.dwSize=sizeof(fx);
    for(iteration=0;iteration<3;++iteration){
        if(!query(&before)){say("FAIL GPU query before DirectDraw\r\n");goto done;}
        fx.dwFillColor=bpp==16?0x5aed:0x7f12abef;
        hr=IDirectDrawSurface_Blt(surface,NULL,NULL,NULL,DDBLT_COLORFILL|DDBLT_WAIT,&fx);
        if(FAILED(hr)){hex("FAIL full Blt=",hr);goto done;}
        if(!check_pixels(surface,0,bpp)){say("FAIL full fill pixels\r\n");goto done;}
        if(!dirty_edges(surface,bpp)){say("FAIL CPU dirty edges\r\n");goto done;}
        fx.dwFillColor=bpp==16?0xe367:0x2567c319;
        hr=IDirectDrawSurface_Blt(surface,&rectangle,NULL,NULL,DDBLT_COLORFILL|DDBLT_WAIT,&fx);
        if(FAILED(hr)){hex("FAIL rectangle Blt=",hr);goto done;}
        if(!check_pixels(surface,1,bpp)){say("FAIL rectangle/cache coherence pixels\r\n");goto done;}
        if(!query(&after)){say("FAIL GPU query after DirectDraw\r\n");goto done;}
        if(after.resets!=before.resets || after.submitted-before.submitted!=2 ||
           after.completed!=after.submitted || after.user_3d_submits!=before.user_3d_submits){
            hex("submitted_delta=",after.submitted-before.submitted);
            hex("user3d_delta=",after.user_3d_submits-before.user_3d_submits);
            say("FAIL: DirectDraw did not complete exactly two hardware fills\r\n");goto done;
        }
    }
    hex("tested_bpp=",bpp);hex("hardware_fills=",6);
    say("PASS: GPU full/rectangle fills, exact pixels, dirty CPU edges and no application 3D tag\r\n");result=0;
done:
    if(source)IDirectDrawSurface_Release(source);
    if(surface)IDirectDrawSurface_Release(surface);
    if(dd){if(fullscreen)IDirectDraw_RestoreDisplayMode(dd);IDirectDraw_SetCooperativeLevel(dd,window,DDSCL_NORMAL);IDirectDraw_Release(dd);}
    if(window)DestroyWindow(window);
    ExitProcess(result);
}
