#include <stdio.h>
#include <stdlib.h>
#include "../app/src/main/cpp/voice.h"
int main(){ FILE*f=fopen("voice_test.raw","rb"); fseek(f,0,2); long n=ftell(f)/4; rewind(f);
 float*x=malloc(n*4); fread(x,4,n,f);
 for(long s=VOICE_N; s<n; s+=48000/4){ VoiceInfo v=VoiceAnalyze(x+s-VOICE_N,48000);
  printf("t=%5.2f rms=%.3f voiced=%d f0=%6.1f harm=%5.2f ot=%6.0f prom=%5.1f\n",s/48000.0,v.rms,v.voiced,v.f0,v.otHarm,v.otFreq,v.otProm);} }
