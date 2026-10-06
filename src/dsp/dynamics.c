#include <math.h>
#include <stdlib.h>
#include "ladspa-abi.h"

/* One instance per audio channel. Controls are shared by the stereo graph;
 * detectors and gain envelopes are independent. No allocation/locking in run. */
struct dynamics {
    float *ports[10];
    float rate, envelope, gain, desired;
    unsigned phase;
};
static LADSPA_Handle instantiate(const LADSPA_Descriptor *descriptor, unsigned long rate) {
    struct dynamics *d = calloc(1, sizeof(*d));
    if (d) { d->rate = rate ? rate : 48000; d->gain = d->desired = 1; }
    return d;
}
static void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    struct dynamics *d = handle;
    if (port < 10) d->ports[port] = data;
}
static void activate(LADSPA_Handle handle) {
    struct dynamics *d = handle;
    d->envelope = 0; d->gain = d->desired = 1; d->phase = 0;
}
static float control(struct dynamics *d, unsigned port, float def, float min, float max) {
    float value = d->ports[port] ? *d->ports[port] : def;
    return isfinite(value) ? fminf(max, fmaxf(min, value)) : def;
}
static float coefficient(struct dynamics *d, float milliseconds) { return expf(-1000.f / (milliseconds * d->rate)); }
static void compressor_run(LADSPA_Handle handle, unsigned long count) {
    struct dynamics *d = handle;
    if (!d->ports[0] || !d->ports[1]) return;
    float threshold = control(d,2,-18,-60,0), ratio = control(d,3,4,1,20);
    float attack = coefficient(d,control(d,4,10,.1,200)), release = coefficient(d,control(d,5,150,10,2000));
    float knee = control(d,6,6,0,24), makeup = powf(10,control(d,7,0,0,24)/20);
    float mix = control(d,8,1,0,1), slope = 1 - 1/ratio;
    for (unsigned long i=0;i<count;i++) {
        float input = d->ports[0][i]; if (!isfinite(input)) input=0;
        d->envelope = fmaxf(fabsf(input), d->envelope * release);
        /* Envelope-to-dB conversions at 1/16 sample rate keep this affordable
         * on Cortex-A7 while gain smoothing still runs at the sample rate. */
        if (d->phase++ % 16 == 0) {
            float over = 20 * log10f(fmaxf(d->envelope,1e-12f)) - threshold, reduction=0;
            if (knee>0 && over>-knee/2 && over<knee/2) reduction=slope*(over+knee/2)*(over+knee/2)/(2*knee);
            else if (over>=knee/2) reduction=slope*fmaxf(0,over);
            d->desired=powf(10,-reduction/20);
        }
        float smooth=d->desired<d->gain?attack:release;
        d->gain=d->desired+(d->gain-d->desired)*smooth;
        float output=input*((1-mix)+mix*d->gain*makeup);
        d->ports[1][i]=isfinite(output)?output:0;
    }
}
static void cleanup(LADSPA_Handle handle) { free(handle); }
static void limiter_run(LADSPA_Handle handle, unsigned long count) {
    struct dynamics *d = handle;
    if (!d->ports[0] || !d->ports[1]) return;
    float ceiling=powf(10,control(d,2,-1,-24,0)/20);
    float release=coefficient(d,control(d,3,100,10,2000));
    float input_gain=powf(10,control(d,4,0,0,24)/20);
    for(unsigned long i=0;i<count;i++) {
        float input=d->ports[0][i]*input_gain;if(!isfinite(input))input=0;
        float peak=fabsf(input),required=peak>ceiling?ceiling/peak:1;
        /* Instant gain reduction enforces the sample ceiling without delay.
         * Release restores unity smoothly; this is not an oversampled limiter. */
        d->gain=fminf(required,1+(d->gain-1)*release);
        d->ports[1][i]=fminf(ceiling,fmaxf(-ceiling,input*d->gain));
    }
}
#define AUDIO_IN (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO)
#define AUDIO_OUT (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO)
#define CONTROL (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL)
#define RANGE(low,high) {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, low, high}
static const LADSPA_PortDescriptor compressor_ports[] = {AUDIO_IN,AUDIO_OUT,CONTROL,CONTROL,CONTROL,CONTROL,CONTROL,CONTROL,CONTROL};
static const char *const compressor_names[] = {"In","Out","Threshold dB","Ratio","Attack ms","Release ms","Knee dB","Makeup dB","Mix"};
static const LADSPA_PortRangeHint compressor_ranges[] = {{0},{0},RANGE(-60,0),RANGE(1,20),RANGE(.1,200),RANGE(10,2000),RANGE(0,24),RANGE(0,24),RANGE(0,1)};
static const LADSPA_Descriptor compressor = {
    .UniqueID=0x504d01,.Label="pm_compressor",.Properties=LADSPA_PROPERTY_HARD_RT_CAPABLE,
    .Name="PipeMixer peak compressor",.Maker="PipeMixer",.Copyright="GPL-3.0-or-later",
    .PortCount=9,.PortDescriptors=compressor_ports,.PortNames=compressor_names,.PortRangeHints=compressor_ranges,
    .instantiate=instantiate,.connect_port=connect_port,.activate=activate,.run=compressor_run,.cleanup=cleanup,
};
static const LADSPA_PortDescriptor limiter_ports[] = {AUDIO_IN,AUDIO_OUT,CONTROL,CONTROL,CONTROL};
static const char *const limiter_names[] = {"In","Out","Ceiling dB","Release ms","Input dB"};
static const LADSPA_PortRangeHint limiter_ranges[] = {{0},{0},RANGE(-24,0),RANGE(10,2000),RANGE(0,24)};
static const LADSPA_Descriptor limiter = {
    .UniqueID=0x504d02,.Label="pm_limiter",.Properties=LADSPA_PROPERTY_HARD_RT_CAPABLE,
    .Name="PipeMixer sample peak limiter",.Maker="PipeMixer",.Copyright="GPL-3.0-or-later",
    .PortCount=5,.PortDescriptors=limiter_ports,.PortNames=limiter_names,.PortRangeHints=limiter_ranges,
    .instantiate=instantiate,.connect_port=connect_port,.activate=activate,.run=limiter_run,.cleanup=cleanup,
};
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) { return index == 0 ? &compressor : index == 1 ? &limiter : NULL; }
