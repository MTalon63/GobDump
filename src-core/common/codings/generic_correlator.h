#pragma once

#include <cstdint>
#include "types.h"
#include <volk/volk_alloc.hh>
#include "common/dsp/demod/constellation.h"
#include "core/opencl.h"

/*
A generic correlator, for (mostly) CCSDS uses
*/
class CorrelatorGeneric
{
private:
    const dsp::constellation_type_t d_modulation;

    int syncword_length;

    float *converted_buffer;
    std::vector<volk::vector<float>> syncwords;

    // Normalized-correlation threshold. The best correlation is normalized by the
    // product of the syncword and received-window norms so a perfect match ~= 1.0
    // and pure noise ~= 0.0. last_locked is set when the best match exceeds this.
    const float d_threshold;

    // Result of the last correlate() call.
    bool last_locked = false;
    float last_corr_norm = 0.0f;

    void rotate_float_buf(float *buf, int size, float rot_deg);
    void modulate_soft(float *buf, uint8_t *bit, int size);

    bool use_gpu = false;

#ifdef USE_OPENCL1
    // OpenCL stuff
    cl_program corr_program;
    cl_command_queue cl_queue;
    cl_kernel corr_kernel;

    cl_mem buffer_syncs;
    cl_mem buffer_input;
    cl_mem buffer_corrs;
    cl_mem buffer_matches;
    cl_mem buffer_nsyncs;
    cl_mem buffer_syncsize;

    float *corro;
    int *matcho;
#endif

public:
    CorrelatorGeneric(dsp::constellation_type_t mod, std::vector<uint8_t> syncword, int max_frm_size, float threshold = 0.5f);
    ~CorrelatorGeneric();

    // Search the frame for the syncword. Returns the best-match position and sets
    // phase/swap/cor. search_start/search_len optionally restrict the search to a
    // window (search_len < 0 means "to the end of the frame"); the GPU path always
    // searches the whole frame. last_locked reflects whether the best normalized
    // correlation exceeded the threshold.
    int correlate(int8_t *soft_input, phase_t &phase, bool &swap, float &cor, int length, int search_start = 0, int search_len = -1);

    // Whether the last correlate() call's best match exceeded the threshold.
    bool locked() const { return last_locked; }
    // Normalized correlation (0..1) of the last correlate() call's best match.
    float last_normalized_corr() const { return last_corr_norm; }
};