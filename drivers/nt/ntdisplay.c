/* SPDX-License-Identifier: GPL-3.0-or-later
 * Guest-only NT5 diagnostic through GDI -> display -> miniport -> WDM.
 * Does not open the WDM device itself or write a diagnostic bitmap.
 */
#include <windows.h>
#include "display/wgpu_display.h"
static HANDLE output;
static void say(const char *s) { DWORD n; WriteFile(output,s,lstrlenA(s),&n,NULL); }
static void hex(const char *label,DWORD value) {
    char b[11]; static const char digits[]="0123456789abcdef";
    unsigned i; say(label);for(i=0;i<8;++i)b[i]=digits[(value>>(28-4*i))&15];
    b[8]='\r';b[9]='\n';b[10]=0;say(b);
}
void __attribute__((stdcall,noreturn)) mainCRTStartup(void)
{
    WgpuDisplayRequest request={sizeof(request),WGPU_DISPLAY_ABI,WGPU_DISPLAY_QUERY,0};
    DWORD bytes,escape=WGPU_DISPLAY_ESCAPE,result=1;
    int invalid=0;
    WgpuDisplayReply *reply;
    HDC dc;
    char *command=GetCommandLineA();
    output=GetStdHandle(STD_OUTPUT_HANDLE);
    /* Deliberately only one optional, fixed operation; reject unknown args. */
    if (*command=='"') { ++command;while(*command && *command!='"')++command;if(*command)++command; }
    else while(*command && *command!=' ')++command;
    while(*command==' ')++command;
    if (*command) {
        if(!lstrcmpA(command,"--triangle"))request.operation=WGPU_DISPLAY_TRIANGLE;
        else if(!lstrcmpA(command,"--draw"))request.operation=WGPU_DISPLAY_DRAW_TEST;
        else if(!lstrcmpA(command,"--selftest"))request.operation=WGPU_DISPLAY_SELFTEST;
        else if(!lstrcmpA(command,"--invalid"))invalid=1;
        else { say("Usage: NTDISP [--selftest|--triangle|--draw|--invalid]\r\n");ExitProcess(2); }
    }
    bytes=sizeof(*reply)+wgpu_display_payload(request.operation);
    reply=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes);
    dc=CreateDCA("DISPLAY",NULL,NULL,NULL);
    if(!reply || !dc) {say("FAIL: cannot allocate reply or create display DC\r\n");goto done;}
    if(ExtEscape(dc,QUERYESCSUPPORT,sizeof(escape),(LPCSTR)&escape,0,NULL)<=0) {
        say("FAIL: display driver has no WGPU escape\r\n");goto done;
    }
    if(invalid) {
        WgpuDisplayRequest bad=request;
        bad.reserved=1;
        if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(bad),(LPCSTR)&bad,bytes,(LPSTR)reply)>0)goto invalid_fail;
        bad=request;bad.abi_version++;
        if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(bad),(LPCSTR)&bad,bytes,(LPSTR)reply)>0)goto invalid_fail;
        bad=request;bad.operation=99;
        if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(bad),(LPCSTR)&bad,bytes,(LPSTR)reply)>0)goto invalid_fail;
        if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(request)-1,(LPCSTR)&request,bytes,(LPSTR)reply)>0)goto invalid_fail;
        if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(request),(LPCSTR)&request,bytes-1,(LPSTR)reply)>0)goto invalid_fail;
        say("PASS: malformed display requests rejected\r\n");
    }
    if(ExtEscape(dc,WGPU_DISPLAY_ESCAPE,sizeof(request),(LPCSTR)&request,bytes,(LPSTR)reply)<=0) {
        say("FAIL: display/miniport request rejected\r\n");goto done;
    }
    hex("transport_status=",reply->status);
    if(reply->size!=bytes || reply->abi_version!=WGPU_DISPLAY_ABI || reply->status ||
        reply->bytes!=bytes-sizeof(*reply)) {say("FAIL: transport reply\r\n");goto done;}
    if(request.operation==WGPU_DISPLAY_TRIANGLE || request.operation==WGPU_DISPLAY_DRAW_TEST) {
        WgpuNtTriangle *image=(WgpuNtTriangle *)(reply+1);
        int draw=request.operation==WGPU_DISPLAY_DRAW_TEST;
        if(!(draw ? wgpu_nt_check_draw_test(image) : wgpu_nt_check_triangle(image))) {
            say("FAIL: GPU pixel comparison\r\n");goto done;
        }
        hex("fence=",image->fence);
        say(draw ? "PASS: two GPU triangles in one batch, exact RGB565 and background\r\n" :
                   "PASS: GDI/display/miniport/WDM hardware triangle and pixel comparison\r\n");
    } else {
        WgpuNtInfo *info=(WgpuNtInfo *)(reply+1);
        if(info->size!=sizeof(*info)||info->abi_version!=WGPU_NT_ABI_VERSION||
            info->magic!=WGPU_NT_MAGIC||info->protocol!=WGPU_NT_PROTOCOL||
            !(info->status&1)||(info->status&2)) {say("FAIL: WGPU query\r\n");goto done;}
        hex("gpu_id=",info->gpu_id);hex("submitted=",info->submitted);
        hex("completed=",info->completed);hex("resets=",info->resets);
        if(request.operation==WGPU_DISPLAY_SELFTEST &&
            (info->smoke_result!=1||info->smoke_value!=info->smoke_expected)) {
            say("FAIL: GPU selftest\r\n");goto done;
        }
        say("PASS: GDI/display/miniport/WDM query\r\n");
    }
    result=0;
    goto done;
invalid_fail:
    say("FAIL: malformed display request accepted\r\n");
done:
    if(dc)DeleteDC(dc);if(reply)HeapFree(GetProcessHeap(),0,reply);ExitProcess(result);
}
