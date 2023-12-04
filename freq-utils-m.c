#include "freq-utils-m.h"

static char *chann_array[] = {"ECPU0","ECPU1","ECPU2","ECPU3","PCPU0","PCPU1","PCPU2","PCPU3","ECPU","PCPU"};
static float e_array[] = {600, 912, 1284, 1752, 2004, 2256, 2424};
static float p_array[] = {660, 924, 1188, 1452, 1704, 1968, 2208, 2400, 2568, 2724, 2868, 2988, 3096, 3204, 3324, 3408, 3504};
static float *freq_state_cores[] = {e_array, p_array};
static unit_data *unit;
static int num_cores;

#define TIME_BETWEEN_MEASUREMENTS 10000000L // 10 millisecond

/**
 * Initialize the channel subscriptions so we can continue to sample over them throughout the
 * experiments. 
*/
void init_unit_data(){
    // Allocate memory and get number of cores
    unit = malloc(sizeof(unit_data));
    num_cores = get_core_num();

    //Initialize channels
    unit->cpu_chann = IOReportCopyChannelsInGroup(CFSTR("CPU Stats"), 0, 0, 0, 0);
    unit->energy_chann = IOReportCopyChannelsInGroup(CFSTR("Energy Model"), 0, 0, 0, 0);

    // Create subscription
    unit->cpu_sub  = IOReportCreateSubscription(NULL, unit->cpu_chann, &unit->cpu_sub_chann, 0, 0);
    unit->pwr_sub  = IOReportCreateSubscription(NULL, unit->energy_chann, &unit->pwr_sub_chann, 0, 0);
    CFRelease(unit->cpu_chann);
    CFRelease(unit->energy_chann);
}

/*
    It might be better here to just take a sample and extract values from that. I don't know however if those
    samples have the same clock time or if they are measured in something else. 
*/
sample_deltas *sample() {
    CFDictionaryRef cpusamp_a  = IOReportCreateSamples(unit->cpu_sub, unit->cpu_sub_chann, NULL);
    nanosleep((const struct timespec[]){{0, TIME_BETWEEN_MEASUREMENTS}}, NULL);
    CFDictionaryRef cpusamp_b  = IOReportCreateSamples(unit->cpu_sub, unit->cpu_sub_chann, NULL);
    CFDictionaryRef cpu_delta  = IOReportCreateSamplesDelta(cpusamp_a, cpusamp_b, NULL);

    // Power should not be delta
    CFDictionaryRef pwr_sample  = IOReportCreateSamples(unit->pwr_sub, unit->pwr_sub_chann, NULL);

    // Done with these
    CFRelease(cpusamp_a);
    CFRelease(cpusamp_b);
    
    sample_deltas *deltas = (sample_deltas *) malloc(sizeof(sample_deltas));
    deltas->cpu_delta = cpu_delta;
    deltas->pwr_sample = pwr_sample;
    return deltas;
}

/*
 * This updates cpu_data with all core/complex information
*/
void get_state_residencies(CFDictionaryRef cpu_delta, cpu_data *data){
    __block int i = 0;
    IOReportIterate(cpu_delta, ^int(IOReportSampleRef sample) {
        // Check sample group and only expand if CPU Stats
        CFStringRef group = IOReportChannelGetGroup(sample);
        if (CFStringCompare(group, CFSTR("CPU Stats"), 0) == kCFCompareEqualTo){
            // Get subgroup (core or complex) and chann_name (core/complex id)
            CFStringRef subgroup    = IOReportChannelGetSubGroup(sample);
            CFStringRef chann_name  = IOReportChannelGetChannelName(sample);
            // Only take these
            if (CFStringCompare(subgroup, CFSTR("CPU Core Performance States"), 0) == kCFCompareEqualTo || 
                CFStringCompare(subgroup, CFSTR("CPU Complex Performance States"), 0) == kCFCompareEqualTo){
                // Only save the CPU-specific samples
                // Note: this will not work for some M1 and M2
                if (CFStringFind(chann_name, CFSTR("CPU"), 0).location != kCFNotFound){
                    // -1 for IDLE
                    int sample_ct = IOReportStateGetCount(sample) - 1;
                    data->core_labels[i] = chann_name;
                    data->num_dvfs_states[i] = sample_ct;
                    data->residencies[i] = malloc(sample_ct*sizeof(uint64_t));
                    // Indicies represent samples for each DVFS state, skip IDLE
                    int ii = 0;
                    for (int iii = 0; iii < IOReportStateGetCount(sample); iii++) {
                        CFStringRef idx_name    = IOReportStateGetNameForIndex(sample, iii);
                        uint64_t residency      = IOReportStateGetResidency(sample, iii);
                        if (CFStringFind(idx_name, CFSTR("IDLE"), 0).location != kCFCompareEqualTo){
                            data->residencies[i][ii] = residency;
                            ii++;
                        }
                    }
                    i++;
                }
            }

        }
        return kIOReportIterOk;
    });
    if (cpu_delta != NULL) CFRelease(cpu_delta);
}

/*
    Averages the active frequency of CPU core of complex based on residencies 
    at DVFS states from get_state_residencies(). Seperates E and P cores/complexes
    DVFS table is hardcoded and will change depending on the system
*/
void get_frequency(CFDictionaryRef cpu_delta, cpu_data *data){
    // Caller responsible to deallocate
    data->core_labels = malloc(num_cores * sizeof(CFStringRef));
    data->frequencies = malloc(num_cores * sizeof(uint64_t));
    data->residencies = malloc(num_cores * sizeof(uint64_t*));
    data->num_dvfs_states = malloc(num_cores * sizeof(uint64_t));

    // Take sample and fill in residency table
    get_state_residencies(cpu_delta, data);
    
    // Loop through residency table and average for each complex/core
    for (int i = 0; i < num_cores; i++){
        // Get DVFS states
        uint64_t num_states = data->num_dvfs_states[i];
        uint64_t sum = 0;
        float freq = 0;
        // Hardcoded, change soon
        int table_idx = 1;
        if (num_states < 10){
            table_idx = 0;
        }
        for (int ii = 0; ii < num_states; ii++){
            sum += data->residencies[i][ii];
        }
        // Take average
        for (int ii = 0; ii < num_states; ii++){
            float percent = (float)data->residencies[i][ii]/sum;
            freq += (percent*freq_state_cores[table_idx][ii]);
        }
        // Save to data struct
        data->frequencies[i] = freq;
    }
}

/*
 * Takes a sample and a core id and returns cpu power
 * 
*/
void get_power(CFDictionaryRef pwr_sample, cpu_data *data){
    data->pwr = malloc(num_cores * sizeof(float));
    __block int i = 0;
    // Get number of indicies 8 or 18 depending on E vs. P
    IOReportIterate(pwr_sample, ^int(IOReportSampleRef sample) {
        CFStringRef chann_name  = IOReportChannelGetChannelName(sample);
        CFStringRef group       = IOReportChannelGetGroup(sample);
        long      value       = IOReportSimpleGetIntegerValue(sample, 0);
        //CFStringRef units =  IOReportChannelGetUnitLabel(sample);
        if (CFStringCompare(group, CFSTR("Energy Model"), 0) == kCFCompareEqualTo) {
            if (i < num_cores){
                CFStringRef core_id_str = CFStringCreateWithCString(NULL, chann_array[i], kCFStringEncodingUTF8);
                if (CFStringCompare(chann_name, core_id_str, 0) == kCFCompareEqualTo){
                    data->pwr[i] = ((float)value);
                    i++;
                }
            }
        } 
        return kIOReportIterOk;
    });
    if (pwr_sample != NULL) CFRelease(pwr_sample);
}

// Change to syscall
// 4+4 E/P cores + 2 complexes
int get_core_num(){
    return 10;
}


int main(int argc, char* argv[]) {
    cpu_data *data = malloc(sizeof(cpu_data));

    // initialize cmd and cpu data
    init_unit_data();
    sample_deltas *deltas = sample();
    get_frequency(deltas->cpu_delta, data);
    get_power(deltas->pwr_sample, data);
    
    // Print out measurements
    for(int i = 0; i<num_cores; i++){
        CFShow(data->core_labels[i]);
        printf("%f\n", data->pwr[i]);
        printf("%llu\n\n", data->frequencies[i]);
    }

    // Clean up
    for (int i = 0; i < num_cores; i++){
        free(data->residencies[i]);
    }
    free(data->residencies);
    free(data->core_labels);
    free(data->num_dvfs_states);
    free(data->pwr);
    
    free(data);
    free(deltas);
}