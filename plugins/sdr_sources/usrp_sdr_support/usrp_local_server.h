#pragma once

#include "common/dsp_source_sink/dsp_sample_source.h"
#include <vector>

namespace usrp_local_server
{
    std::vector<dsp::SourceDescriptor> getSources();
    void stop();
}
