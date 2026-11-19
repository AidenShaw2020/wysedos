/* Host-only API mocks; actual transport.c is included by the runner.
 * This verifies lifetime/control flow, not Windows ABI or kernel execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned long ULONG;
typedef int32_t NTSTATUS;
typedef void *PVOID;
typedef unsigned char BOOLEAN;
typedef wchar_t WCHAR;
typedef struct {const WCHAR *Buffer;} UNICODE_STRING;
typedef struct {int unused;} FILE_OBJECT,*PFILE_OBJECT,DEVICE_OBJECT,*PDEVICE_OBJECT,KEVENT;
typedef struct {PFILE_OBJECT FileObject;} STACK;
typedef struct {STACK stack;} IRP,*PIRP;
typedef struct {NTSTATUS Status;uintptr_t Information;} IO_STATUS_BLOCK;
#define NTAPI
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_PENDING ((NTSTATUS)0x103)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_DEVICE_PROTOCOL_ERROR ((NTSTATUS)0xc0000186)
#define PASSIVE_LEVEL 0
#define FILE_READ_DATA 1
#define FILE_WRITE_DATA 2
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define FALSE 0
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset(p,0,n)
static FILE_OBJECT file;
static DEVICE_OBJECT device;
static IRP irp;
static unsigned irql,opens,refs,builds,calls,waits,derefs;
static int allocation_failure,pending,length_delta;
static NTSTATUS open_status,complete_status;
static ULONG expected_code,expected_length;
static ULONG expected_input_length;
static const void *expected_input;
static IO_STATUS_BLOCK *saved_iosb;
static PVOID saved_output;
static unsigned KeGetCurrentIrql(void) {return irql;}
static void RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *v) {s->Buffer=v;}
static NTSTATUS IoGetDeviceObjectPointer(UNICODE_STRING *s,ULONG access,PFILE_OBJECT *f,PDEVICE_OBJECT *d) {
    ++opens;assert(!wcscmp(s->Buffer,L"\\Device\\WyseWgpuNT"));assert(access==3);
    if(!NT_SUCCESS(open_status))return open_status;
    ++refs;*f=&file;*d=&device;return STATUS_SUCCESS;
}
static void KeInitializeEvent(KEVENT *e,unsigned t,int signaled) {(void)e;assert(t==0&&!signaled);}
static PIRP IoBuildDeviceIoControlRequest(ULONG code,PDEVICE_OBJECT d,PVOID in,ULONG in_bytes,
    PVOID out,ULONG out_bytes,int internal,KEVENT *event,IO_STATUS_BLOCK *iosb) {
    ++builds;assert(refs==1&&d==&device&&code==expected_code&&in_bytes==expected_input_length);
    if(expected_input_length)assert(in&&!memcmp(in,expected_input,expected_input_length));
    else assert(!in);
    assert((out != NULL) == (expected_length != 0));
    assert(out_bytes==expected_length&&!internal&&event);
    saved_iosb=iosb;saved_output=out;irp.stack.FileObject=NULL;
    return allocation_failure?NULL:&irp;
}
static STACK *IoGetNextIrpStackLocation(PIRP p) {assert(p==&irp);return &p->stack;}
static void complete(void) {
    saved_iosb->Status=complete_status;
    saved_iosb->Information=expected_length+length_delta;
    /* Include partial/failed output to ensure the transport scrubs it. */
    if (expected_length) memset(saved_output,0x5a,expected_length);
}
static NTSTATUS IoCallDriver(PDEVICE_OBJECT d,PIRP p) {
    ++calls;assert(refs==1&&d==&device&&p==&irp&&p->stack.FileObject==&file);
    if(pending)return STATUS_PENDING;complete();return complete_status;
}
static NTSTATUS KeWaitForSingleObject(KEVENT *e,unsigned reason,unsigned mode,int alert,PVOID timeout) {
    ++waits;assert(e&&reason==Executive&&mode==KernelMode&&!alert&&!timeout&&refs==1);
    complete();return STATUS_SUCCESS;
}
static void ObDereferenceObject(PFILE_OBJECT f) {assert(f==&file&&refs==1);--refs;++derefs;}
#include "actual_transport.c"

static void reset(void) {
    irql=opens=refs=builds=calls=waits=derefs=0;
    allocation_failure=pending=length_delta=0;
    open_status=complete_status=STATUS_SUCCESS;
    expected_input_length=0;expected_input=NULL;
}
static unsigned char output[sizeof(WgpuDisplayReply)+sizeof(WgpuNtTriangle)+16];
static void request(unsigned operation) {
    WgpuDisplayRequest r={sizeof(r),WGPU_DISPLAY_ABI,operation,0};
    ULONG payload=wgpu_display_payload(operation),n=sizeof(WgpuDisplayReply)+payload;
    WgpuDisplayReply *reply=(WgpuDisplayReply *)output;
    expected_length=payload;
    expected_code=operation==1?WGPU_NT_IOCTL_QUERY:operation==2?WGPU_NT_IOCTL_SELFTEST:
                  operation==3?WGPU_NT_IOCTL_TRIANGLE:WGPU_NT_IOCTL_DRAW_TEST;
    memset(output,0xcc,sizeof(output));
    assert(wgpu_display_send(&r,sizeof(r),output,n)==STATUS_SUCCESS);
    assert(reply->size==n&&reply->abi_version==WGPU_DISPLAY_ABI);
    NTSTATUS expected=irql?STATUS_INVALID_DEVICE_STATE:!NT_SUCCESS(open_status)?open_status:
        allocation_failure?STATUS_INSUFFICIENT_RESOURCES:!NT_SUCCESS(complete_status)?complete_status:
        length_delta?STATUS_DEVICE_PROTOCOL_ERROR:STATUS_SUCCESS;
    assert((NTSTATUS)reply->status==expected);
    assert(reply->bytes==(NT_SUCCESS(expected)?payload:0));
    for(ULONG i=sizeof(*reply);i<n;++i)assert(output[i]==(NT_SUCCESS(expected)?0x5a:0));
    for(ULONG i=n;i<sizeof(output);++i)assert(output[i]==0xcc);
    assert(!refs);
}
static void fill_request(int alias) {
    WgpuNtFill r={sizeof(r),WGPU_NT_ABI_VERSION,4096,2560,640,480,32,3,4,639,479,0x7f12abef,0};
    ULONG n=sizeof(WgpuDisplayReply)+sizeof(WgpuNtFillResult);
    WgpuDisplayReply *reply=(WgpuDisplayReply *)output;
    expected_input=&r;expected_input_length=sizeof(r);
    expected_code=WGPU_NT_IOCTL_FILL;expected_length=sizeof(WgpuNtFillResult);
    memset(output,0xcc,sizeof(output));
    if(alias)memcpy(output,&r,sizeof(r));
    assert(wgpu_display_fill_send(alias?(WgpuNtFill *)output:&r,sizeof(r),output,n)==STATUS_SUCCESS);
    NTSTATUS expected=irql?STATUS_INVALID_DEVICE_STATE:!NT_SUCCESS(open_status)?open_status:
        allocation_failure?STATUS_INSUFFICIENT_RESOURCES:!NT_SUCCESS(complete_status)?complete_status:
        length_delta?STATUS_DEVICE_PROTOCOL_ERROR:STATUS_SUCCESS;
    assert(reply->size==n&&reply->abi_version==WGPU_DISPLAY_ABI&&(NTSTATUS)reply->status==expected);
    assert(reply->bytes==(NT_SUCCESS(expected)?sizeof(WgpuNtFillResult):0));
    for(ULONG i=sizeof(*reply);i<n;++i)assert(output[i]==(NT_SUCCESS(expected)?0x5a:0));
    for(ULONG i=alias?sizeof(r):n;i<sizeof(output);++i)assert(output[i]==0xcc);
    assert(!refs);
}
static void draw_request(void) {
    WgpuNtDraw r={0};
    WgpuDisplayReply *reply=(WgpuDisplayReply *)output;
    ULONG n=sizeof(*reply)+sizeof(WgpuNtFillResult);
    r.target=(WgpuNtFill){sizeof(r.target),WGPU_NT_ABI_VERSION,4096,64,16,16,32,0,0,16,16,0,0};
    r.size=sizeof(r);
    r.vertex_count=3;
    expected_input=&r;expected_input_length=sizeof(r);
    expected_code=WGPU_NT_IOCTL_DRAW;expected_length=sizeof(WgpuNtFillResult);
    memcpy(output,&r,sizeof(r));
    assert(wgpu_display_draw_send((WgpuNtDraw *)output,sizeof(r),output,n)==STATUS_SUCCESS);
    assert(reply->size==n && reply->abi_version==WGPU_DISPLAY_ABI);
    assert((NTSTATUS)reply->status==(length_delta?STATUS_DEVICE_PROTOCOL_ERROR:STATUS_SUCCESS));
    assert(reply->bytes==(length_delta?0:sizeof(WgpuNtFillResult)));
    for(ULONG i=sizeof(*reply);i<n;++i)assert(output[i]==(length_delta?0:0x5a));
    assert(!memcmp(output+n,(const unsigned char *)&r+n,sizeof(r)-n));
    reset();
    r.vertices[0].x=0x7f800000;
    memcpy(output,&r,sizeof(r));
    assert(wgpu_display_draw_send((WgpuNtDraw *)output,sizeof(r),output,n)==STATUS_INVALID_PARAMETER);
    assert(!opens && !memcmp(output,&r,sizeof(r)));
}
#ifdef WGPU_NT_PIPELINE
static void pipeline_requests(void)
{
    struct { WgpuNtMaterialGroup group; WgpuNtMaterialDraw draw; } packet = {0};
    WgpuDisplayReply *reply = (WgpuDisplayReply *)output;
    ULONG n = sizeof(*reply) + sizeof(WgpuNtFillResult);
    packet.draw.size = sizeof(packet.draw);
    packet.draw.vertex_count = 3;
    packet.group.count = 1;
    packet.group.size = sizeof(packet.group) + wgpu_nt_material_bytes(3);
    reset();
    expected_code = WGPU_NT_IOCTL_QUEUE_MATERIAL;
    expected_input = &packet;
    expected_input_length = packet.group.size;
    expected_length = sizeof(WgpuNtFillResult);
    pending = 1;
    assert(wgpu_display_pipeline_send(WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL, &packet,
        packet.group.size, output, n) == STATUS_SUCCESS);
    assert(waits == 1 && !refs && reply->status == STATUS_SUCCESS && reply->bytes == expected_length);
    reset();
    assert(wgpu_display_pipeline_send(WGPU_DISPLAY_QUEUE_MATERIAL_IOCTL, &packet,
        packet.group.size - 1, output, n) == STATUS_INVALID_PARAMETER);
    assert(!opens);

    reset();
    assert(wgpu_display_pipeline_send(WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL,
        NULL, 0, output, n) == STATUS_INVALID_PARAMETER);
    assert(!opens);
    expected_code = WGPU_NT_IOCTL_DRAIN_MATERIAL;
    expected_length = 0;
    pending = 1;
    complete_status = STATUS_UNSUCCESSFUL;
    assert(wgpu_display_pipeline_send(WGPU_DISPLAY_DRAIN_MATERIAL_IOCTL,
        NULL, 0, NULL, 0) == STATUS_UNSUCCESSFUL);
    assert(waits == 1 && !refs && derefs == 1);
}
#endif

int main(void) {
#ifdef WGPU_NT_PIPELINE
    pipeline_requests();
#endif
    reset();pending=1;draw_request();
    reset();length_delta=-1;draw_request();
    for(unsigned op=1;op<=4;++op) {
        reset();request(op);assert(opens==1&&builds==1&&calls==1&&!waits&&derefs==1);
        reset();pending=1;request(op);assert(waits==1&&derefs==1);
        reset();open_status=STATUS_UNSUCCESSFUL;request(op);assert(opens==1&&!builds&&!derefs);
        reset();allocation_failure=1;request(op);assert(builds==1&&!calls&&derefs==1);
        reset();complete_status=STATUS_UNSUCCESSFUL;request(op);assert(calls==1&&derefs==1);
        reset();pending=1;complete_status=STATUS_UNSUCCESSFUL;request(op);assert(waits==1&&derefs==1);
        reset();length_delta=-1;request(op);
        reset();length_delta=1;request(op);
        reset();irql=2;request(op);assert(!opens&&!builds&&!derefs);
    }
    WgpuDisplayRequest r={sizeof(r),WGPU_DISPLAY_ABI,WGPU_DISPLAY_QUERY,0};
    ULONG n=sizeof(WgpuDisplayReply)+sizeof(WgpuNtInfo);
    for(unsigned bad=0;bad<7;++bad) {
        reset();memset(output,0xcc,sizeof(output));WgpuDisplayRequest b=r;
        ULONG in=sizeof(r),out=n;
        if(bad==0)b.size--;if(bad==1)b.abi_version++;if(bad==2)b.operation=99;
        if(bad==3)b.reserved=1;if(bad==4)in--;if(bad==5)out--;if(bad==6)out++;
        assert(wgpu_display_send(&b,in,output,out)==STATUS_INVALID_PARAMETER);
        assert(!opens&&!builds);for(unsigned i=0;i<sizeof(output);++i)assert(output[i]==0xcc);
    }
    reset();expected_code=WGPU_NT_IOCTL_QUERY;expected_length=sizeof(WgpuNtInfo);
    memcpy(output,&r,sizeof(r));
    assert(wgpu_display_send((WgpuDisplayRequest *)output,sizeof(r),output,n)==STATUS_SUCCESS);
    assert(derefs==1&&!refs); /* METHOD_BUFFERED alias preserves operation */
    reset();fill_request(0);assert(calls==1&&derefs==1);
    reset();pending=1;fill_request(1);assert(waits==1&&derefs==1);
    reset();open_status=STATUS_UNSUCCESSFUL;fill_request(0);assert(!builds&&!derefs);
    reset();allocation_failure=1;fill_request(0);assert(!calls&&derefs==1);
    reset();complete_status=STATUS_UNSUCCESSFUL;fill_request(0);
    reset();pending=1;complete_status=STATUS_UNSUCCESSFUL;fill_request(1);
    reset();length_delta=-1;fill_request(0);
    reset();length_delta=1;fill_request(0);
    reset();irql=2;fill_request(0);assert(!opens);
    WgpuNtFill fill={sizeof(fill),WGPU_NT_ABI_VERSION,0,2560,640,480,32,0,0,640,480,0,0};
    for(unsigned bad=0;bad<6;++bad){
        reset();WgpuNtFill b=fill;memset(output,0xcc,sizeof(output));
        ULONG in=sizeof(b),out=sizeof(WgpuDisplayReply)+sizeof(WgpuNtFillResult);
        if(bad==0)b.reserved=1;if(bad==1)b.offset=1;if(bad==2)b.bpp=24;
        if(bad==3)b.right=641;if(bad==4)in--;if(bad==5)out--;
        assert(wgpu_display_fill_send(&b,in,output,out)==STATUS_INVALID_PARAMETER);
        assert(!opens&&!builds);for(unsigned i=0;i<sizeof(output);++i)assert(output[i]==0xcc);
    }
    puts("PASS: actual NT display transport validation, sync/pending completion, errors, reference lifetime and output isolation");
    return 0;
}
