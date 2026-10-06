#pragma once
/* The LADSPA 1.1 binary interface used by PipeWire's filter-graph host.
 * Only declarations needed by the bundled processors are provided here. */
typedef float LADSPA_Data;
typedef void *LADSPA_Handle;
typedef int LADSPA_PortDescriptor;
typedef int LADSPA_Properties;
typedef int LADSPA_PortRangeHintDescriptor;
typedef struct { LADSPA_PortRangeHintDescriptor HintDescriptor; LADSPA_Data LowerBound, UpperBound; } LADSPA_PortRangeHint;
#define LADSPA_PORT_INPUT 1
#define LADSPA_PORT_OUTPUT 2
#define LADSPA_PORT_CONTROL 4
#define LADSPA_PORT_AUDIO 8
#define LADSPA_PROPERTY_HARD_RT_CAPABLE 4
#define LADSPA_HINT_BOUNDED_BELOW 1
#define LADSPA_HINT_BOUNDED_ABOVE 2
typedef struct LADSPA_Descriptor {
    unsigned long UniqueID;
    const char *Label;
    LADSPA_Properties Properties;
    const char *Name, *Maker, *Copyright;
    unsigned long PortCount;
    const LADSPA_PortDescriptor *PortDescriptors;
    const char *const *PortNames;
    const LADSPA_PortRangeHint *PortRangeHints;
    void *ImplementationData;
    LADSPA_Handle (*instantiate)(const struct LADSPA_Descriptor *, unsigned long);
    void (*connect_port)(LADSPA_Handle, unsigned long, LADSPA_Data *);
    void (*activate)(LADSPA_Handle);
    void (*run)(LADSPA_Handle, unsigned long);
    void (*run_adding)(LADSPA_Handle, unsigned long);
    void (*set_run_adding_gain)(LADSPA_Handle, LADSPA_Data);
    void (*deactivate)(LADSPA_Handle);
    void (*cleanup)(LADSPA_Handle);
} LADSPA_Descriptor;
typedef const LADSPA_Descriptor *(*LADSPA_Descriptor_Function)(unsigned long);
