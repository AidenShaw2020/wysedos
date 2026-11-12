/* Actual audio.c mixer, independent PCM oracle and bounded FIFO service clock. */
#include <stdlib.h>
#include "../../efi/platform/audio.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
int g_audio_volume=16,g_mix_level[MIX_COUNT];
volatile uint64_t g_core_heartbeat[4];
static unsigned now_us,last_feed,max_gap,feeds,samples,put_calls,opl_ready;
static int owner;
static void advance(unsigned us) {now_us+=us;}
void analog_feed(void) {unsigned gap=now_us-last_feed;if(gap>max_gap)max_gap=gap;last_feed=now_us;feeds++;}
bool analog_owns_core(void){return owner!=0;}
void analog_put(const int16_t *frames,int n){CHECK(n==512);CHECK(frames!=NULL);put_calls++;}
uint32_t analog_level(void){return 4096;}
int lpe_pump(void (*fill)(int16_t*,int)){(void)fill;return 0;}
bool lpe_active(void){return true;}
bool mp_start(int c,void(*f)(void)){(void)c;(void)f;return false;}
void mp_hang_watch_core3(void){}
int adlib_synth_step(void){advance(200);opl_ready+=64;return 1;}
uint32_t adlib_ready(AdlibState *s){(void)s;return opl_ready;}
void adlib_getframe(AdlibState *s,int*l,int*r){(void)s;*l=*r=0;}
void sb16_getsample(SB16State*s,int*r,int*l){(void)s;advance(4);*l=(int)samples+100;*r=-(int)samples-200;samples++;}
int16_t pcspk_sample(PCSpkState*s){(void)s;CHECK(0);return 0;}
int16_t sn76489_sample(void){CHECK(0);return 0;}
int16_t dss_sample(void){CHECK(0);return 0;}
int gmsynth_active(void){CHECK(0);return 0;}
void gmsynth_getframe(int*l,int*r){(void)l;(void)r;CHECK(0);}
int gmsynth_step(void){return 0;}
int16_t midi_sample(void){CHECK(0);return 0;}
int main(void){
 PC pc={0};int16_t out[1024];apc=&pc;pc.sb16_enabled=1;
 for(int i=0;i<MIX_COUNT;i++)g_mix_level[i]=16;
 for(int o=0;o<2;o++)for(int opl=0;opl<2;opl++){
  now_us=last_feed=max_gap=feeds=samples=put_calls=opl_ready=0;owner=o;pc.adlib_enabled=opl;
  mix(out,512);
  CHECK(samples==512 && put_calls==1);
  for(unsigned i=0;i<512;i++){CHECK(out[2*i]==(int)i+100);CHECK(out[2*i+1]==-(int)i-200);}
  if(owner)CHECK(feeds==0);else{CHECK(feeds>0);CHECK(max_gap<=333);}
  printf("PASS owner%d OPL%d PCM-identical put-once feeds%u max_gap%uus\n",o,opl,feeds,max_gap);
 }
 return 0;
}
