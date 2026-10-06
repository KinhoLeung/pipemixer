#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsp/ladspa-abi.h"

struct fixture { const LADSPA_Descriptor *plugin; LADSPA_Handle handle; float controls[8], input[256], output[256]; };
static struct fixture create(const LADSPA_Descriptor *plugin) {
    struct fixture f={.plugin=plugin};f.handle=plugin->instantiate(plugin,48000);assert(f.handle);return f;
}
static void connect(struct fixture *f) {
    f->plugin->connect_port(f->handle,0,f->input);f->plugin->connect_port(f->handle,1,f->output);
    for(unsigned i=2;i<f->plugin->PortCount;i++)f->plugin->connect_port(f->handle,i,&f->controls[i-2]);
    f->plugin->activate(f->handle);
}
static float constant(struct fixture *f,float amplitude,unsigned samples) {
    float last=0;
    while(samples){unsigned count=samples<256?samples:256;for(unsigned i=0;i<count;i++)f->input[i]=amplitude;
        f->plugin->run(f->handle,count);for(unsigned i=0;i<count;i++)assert(isfinite(f->output[i]));last=f->output[count-1];samples-=count;}
    return last;
}
static void compressor_test(const LADSPA_Descriptor *plugin) {
    struct fixture f=create(plugin);float values[]={-18,4,10,150,0,0,1};
    for(unsigned i=0;i<7;i++)f.controls[i]=values[i];
    connect(&f);
    float expected=powf(10,(-18+(20*log10f(.5f)+18)/4)/20),compressed=constant(&f,.5,96000);
    assert(fabsf(compressed-expected)<.001);
    f.controls[5]=6;float boosted=constant(&f,.5,48000);assert(fabsf(boosted/compressed-powf(10,.3))<.005);
    f.controls[5]=0;f.controls[6]=.5;float mixed=constant(&f,.5,48000);assert(fabsf(mixed-(.5f+compressed)/2)<.001);
    f.controls[6]=1;f.controls[1]=1;assert(fabsf(constant(&f,.5,96000)-.5f)<.001);
    f.controls[1]=4;assert(fabsf(constant(&f,.01,96000)-.01f)<.0001);
    f.controls[4]=12;f.plugin->activate(f.handle);float knee=constant(&f,powf(10,-18.f/20),96000);assert(knee<powf(10,-18.f/20)*.94);
    f.controls[4]=0;f.controls[2]=1;f.plugin->activate(f.handle);float fast=constant(&f,.5,480);
    f.controls[2]=100;f.plugin->activate(f.handle);float slow=constant(&f,.5,480);assert(fast<slow*.6);
    f.controls[2]=1;f.controls[3]=10;f.plugin->activate(f.handle);constant(&f,.5,48000);float quick=constant(&f,.01,14400);
    f.controls[3]=1000;f.plugin->activate(f.handle);constant(&f,.5,48000);float gradual=constant(&f,.01,14400);assert(quick>.009&&gradual<.006);
    constant(&f,NAN,256);constant(&f,INFINITY,256);
    printf("PASS compressor transfer, makeup, parallel mix, knee, attack/release and non-finite input: %.6f (expected %.6f)\n",compressed,expected);
    f.plugin->cleanup(f.handle);
}
static void limiter_test(const LADSPA_Descriptor *plugin) {
    struct fixture f=create(plugin);f.controls[0]=-12;f.controls[1]=100;connect(&f);
    float ceiling=powf(10,-12.f/20),limited=constant(&f,.5,48000);assert(fabsf(limited-ceiling)<.000001);
    f.plugin->activate(f.handle);assert(fabsf(constant(&f,.01,256)-.01f)<.000001);
    float spike=constant(&f,2,1);assert(fabsf(spike)<=ceiling+.000001);
    float recovered=constant(&f,.01,48000);assert(recovered>.0099);
    f.controls[1]=10;f.plugin->activate(f.handle);constant(&f,2,1);float fast=constant(&f,.01,4800);
    f.controls[1]=1000;f.plugin->activate(f.handle);constant(&f,2,1);float slow=constant(&f,.01,4800);assert(fast>.0099&&slow<.003);
    f.controls[2]=24;unsigned random=1;
    for(unsigned block=0;block<1000;block++) {
        for(unsigned i=0;i<256;i++){random=random*1664525u+1013904223u;f.input[i]=((int)(random>>8)-8388608)/2000000.f;}
        f.plugin->run(f.handle,256);for(unsigned i=0;i<256;i++)assert(isfinite(f.output[i])&&fabsf(f.output[i])<=ceiling+.000001);
    }
    constant(&f,NAN,256);constant(&f,INFINITY,256);
    printf("PASS limiter ceiling, impulses, release, input boost and non-finite input: ceiling %.6f, DC %.6f\n",ceiling,limited);
    f.plugin->cleanup(f.handle);
}
int main(int argc,char **argv) {
    assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW);if(!library){fprintf(stderr,"%s\n",dlerror());return 1;}
    LADSPA_Descriptor_Function descriptor=(LADSPA_Descriptor_Function)dlsym(library,"ladspa_descriptor");assert(descriptor);
    compressor_test(descriptor(0));limiter_test(descriptor(1));dlclose(library);return 0;
}
