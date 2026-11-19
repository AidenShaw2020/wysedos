/* Host-only validation of the actual builder. No GPU/privileged code runs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/nt/wgpu_triangle.c"
static WgpuNtU32 memory[(WGPU_NT_TRI_MIN+4096)/4];
static uint32_t float_bits(float f) {uint32_t n;memcpy(&n,&f,4);return n;}
static WgpuNtFill initial(void)
{
    WgpuNtFill q={sizeof(q),WGPU_NT_ABI_VERSION,4096,2560,640,480,32,
                  3,4,639,479,0x7f12abef,0};return q;
}
static void poison(void){memset(memory,0xa5,sizeof(memory));}
static void unchanged(void)
{for(unsigned i=0;i<sizeof(memory)/sizeof(memory[0]);++i)assert(memory[i]==0xa5a5a5a5ul);}
static void reject(WgpuNtFill *q,uint32_t size,uint32_t base)
{
    poison();assert(!wgpu_nt_build_fill(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,q,size,base));
    unchanged();
}
static unsigned command(uint32_t word)
{
    for(unsigned i=0;i<0x8000/4;++i)if(memory[WGPU_NT_TRI_BATCH/4+i]==word)return WGPU_NT_TRI_BATCH/4+i;
    assert(0);return 0;
}
static void check_draw(WgpuNtFill *q)
{
    unsigned i,at,ss,vb;
    poison();assert(wgpu_nt_build_fill(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,q,8*1024*1024,0x60000000));
    ss=memory[(WGPU_NT_TRI_BATCH+0x8000)/4];
    volatile struct gen8_surface_state *surface=(void *)(memory+(WGPU_NT_TRI_BATCH+ss)/4);
    assert(surface->ss8.base_addr==0x60000000+q->offset);
    assert(surface->ss2.width==q->width-1 && surface->ss2.height==q->height-1);
    assert(surface->ss3.pitch==q->pitch-1);
    assert(surface->ss0.surface_format==(q->bpp==32?SURFACEFORMAT_B8G8R8A8_UNORM:SURFACEFORMAT_B5G6R5_UNORM));
    at=command(GEN4_3DSTATE_VERTEX_BUFFERS|(1+4-2));
    assert(memory[at+4]==6*32);vb=memory[at+2]-0x50000000;
    static const unsigned right[6]={0,1,0,1,1,0},bottom[6]={0,0,1,0,1,1};
    for(i=0;i<6;++i){
        WgpuNtU32 *v=memory+vb/4+i*8;
        assert(v[0]==float_bits((float)(right[i]?q->right:q->left)));
        assert(v[1]==float_bits((float)(bottom[i]?q->bottom:q->top)));
        unsigned r=q->bpp==32?(q->color>>16)&255:q->color>>11;
        unsigned g=q->bpp==32?(q->color>>8)&255:(q->color>>5)&63;
        unsigned b=q->bpp==32?q->color&255:q->color&31;
        assert(v[2]==float_bits((float)r/(q->bpp==32?255:31)));
        assert(v[3]==float_bits((float)g/(q->bpp==32?255:63)));
        assert(v[4]==float_bits((float)b/(q->bpp==32?255:31)));
        assert(v[5]==float_bits(q->bpp==32?(float)(q->color>>24)/255:1.0f));
    }
    at=command(GEN4_3DPRIMITIVE|(7-2));assert(memory[at+2]==6);
    assert(memory[0]==1 && memory[64]==WGPU_NT_TRI_BATCH && memory[66]==2);
    for(i=0;i<sizeof(memory)/sizeof(memory[0]);++i){
        int allowed=i==0||(i>=64&&i<68)||
            (i>=WGPU_NT_TRI_KERNEL/4&&i<WGPU_NT_TRI_KERNEL/4+20)||
            (i>=WGPU_NT_TRI_BATCH/4&&i<(WGPU_NT_TRI_BATCH+WGPU_NT_TRI_BATCH_BYTES)/4);
        if(!allowed)assert(memory[i]==0xa5a5a5a5ul);
    }
}
static void check_copy(void)
{
    WgpuNtCopy q={initial(),initial()}, bad;
    q.src.color=q.dst.color=0;
    q.dst.offset=q.src.offset+q.src.pitch*q.src.height;
    q.dst.pitch=2624;
    q.dst.left++;q.dst.right++;
    poison();
    assert(wgpu_nt_build_copy(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,&q,8*1024*1024,0x60000000));
    unsigned table=(WGPU_NT_TRI_BATCH+STATE_OFFSET)/4;
    const struct gen8_surface_state *src=(void *)(memory+(WGPU_NT_TRI_BATCH+memory[table+1])/4);
    const struct gen8_surface_state *dst=(void *)(memory+(WGPU_NT_TRI_BATCH+memory[table])/4);
    assert(src->ss8.base_addr==0x60000000+q.src.offset && src->ss3.pitch==q.src.pitch-1);
    assert(dst->ss8.base_addr==0x60000000+q.dst.offset && dst->ss3.pitch==q.dst.pitch-1);
    unsigned vb=(memory[command(GEN4_3DSTATE_VERTEX_BUFFERS|3)+2]-0x50000000)/4;
    assert(memory[vb]==float_bits(4.0f) && memory[vb+2]==float_bits(3.0f/640));
    assert(memory[vb+3]==float_bits(4.0f/480));
    assert(memory[vb+10]==float_bits(639.0f/640));
    assert(memory[command(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS)+1]);
#define REJECT_COPY(field,value) do { \
    bad=q;bad.field=value;poison(); \
    assert(!wgpu_nt_build_copy(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,&bad,8*1024*1024,0x60000000)); \
    unchanged(); \
} while(0)
    REJECT_COPY(dst.offset,q.dst.offset-64);
    REJECT_COPY(dst.right,q.dst.right-1);
    REJECT_COPY(dst.bpp,16);
    REJECT_COPY(src.color,1);
    REJECT_COPY(src.bottom,0xffffffffu);
#undef REJECT_COPY
}
static void check_draw_packet(void)
{
    WgpuNtDraw q={0}, bad;
    q.target=initial();
    q.target.left=q.target.top=q.target.color=0;
    q.target.right=q.target.width;
    q.target.bottom=q.target.height;
    q.size=sizeof(q);
    q.vertex_count=WGPU_NT_DRAW_MAX_VERTICES;
    for (unsigned i=0; i<q.vertex_count; ++i) {
        q.vertices[i].x=float_bits((float)i-0.5f);
        q.vertices[i].y=float_bits((float)i+0.25f);
        q.vertices[i].argb=0x7f12abef;
    }
    poison();
    assert(wgpu_nt_build_draw(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,&q,8*1024*1024,0x60000000));
    unsigned vb=(memory[command(GEN4_3DSTATE_VERTEX_BUFFERS|3)+2]-0x50000000)/4;
    assert(memory[command(GEN4_3DPRIMITIVE|5)+2]==q.vertex_count);
    assert(memory[WGPU_NT_TRI_TARGET/4]==0xa5a5a5a5ul);
    for (unsigned i=0; i<q.vertex_count; ++i) {
        assert(memory[vb+8*i]==q.vertices[i].x);
        assert(memory[vb+8*i+1]==q.vertices[i].y);
        assert(memory[vb+8*i+2]==float_bits(18.0f/255));
        assert(memory[vb+8*i+5]==float_bits(127.0f/255));
    }
#define REJECT_DRAW(field,value) do { \
    bad=q;bad.field=value;poison(); \
    assert(!wgpu_nt_build_draw(memory,WGPU_NT_TRI_MIN,0x50000000,1,2,1,&bad,8*1024*1024,0x60000000)); \
    unchanged(); \
} while(0)
    REJECT_DRAW(size,0);
    REJECT_DRAW(vertex_count,0);
    REJECT_DRAW(vertex_count,4);
    REJECT_DRAW(vertex_count,99);
    REJECT_DRAW(target.offset,0xfffff000u);
    REJECT_DRAW(vertices[0].x,0x7fc00000u);
    REJECT_DRAW(vertices[95].y,0xff800000u);
    REJECT_DRAW(vertices[95].x,0xc5000001u);
#undef REJECT_DRAW
}

int main(void)
{
    unsigned n,max;
    WgpuNtFill q=initial(),bad;
    for(n=0;n<=2048;++n)assert(integer_float(n)==float_bits((float)n));
    for(max=1;max<=2048;++max)
        for(n=0;n<=max;++n)assert(channel_float(n,max)==float_bits((float)n/max));
    for(max=31;max<=255;max=max==31?63:255){
        for(n=0;n<=max;++n)assert(channel_float(n,max)==float_bits((float)n/max));
        if(max==255)break;
    }
    assert(wgpu_nt_fill_valid(&q,8*1024*1024,0x60000000));
    reject(NULL,8*1024*1024,0x60000000);
#define BAD(field,value) do{bad=q;bad.field=value;reject(&bad,8*1024*1024,0x60000000);}while(0)
    BAD(size,0);BAD(abi_version,2);BAD(reserved,1);BAD(bpp,24);
    BAD(offset,1);BAD(offset,0xfffff000);BAD(pitch,0);BAD(pitch,2559);BAD(pitch,16448);
    BAD(width,0);BAD(width,2049);BAD(width,641);BAD(height,0);BAD(height,2049);
    BAD(left,639);BAD(right,641);BAD(top,479);BAD(bottom,481);
    reject(&q,4096+2560*480-1,0x60000000);
    reject(&q,8*1024*1024,0);reject(&q,8*1024*1024,0x60000001);
    reject(&q,8*1024*1024,0xfffff000);
    bad=q;bad.bpp=16;bad.color=0x10000;reject(&bad,8*1024*1024,0x60000000);
    check_draw(&q);q.bpp=16;q.color=0x5aed;check_draw(&q);
    q.offset=0;q.width=q.height=2048;q.pitch=4096;q.left=q.top=0;q.right=q.bottom=2048;
    assert(wgpu_nt_fill_valid(&q,8*1024*1024,0xff800000));check_draw(&q);
    poison();assert(!wgpu_nt_build_fill(memory,WGPU_NT_TRI_MIN-1,0x50000000,1,2,1,&q,8*1024*1024,0x60000000));unchanged();
    check_copy();
    check_draw_packet();
    puts("PASS: fill, copy and draw bounds, IEEE conversions, vertices and write isolation");return 0;
}
