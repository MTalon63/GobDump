#include "core/plugin.h"
#include "logger.h"
#include "birriasdr_sdr_source.h"

class BirriaSDRSupport : public satdump::Plugin
{
public:
    std::string getID()
    {
        return "birriasdr_sdr_support";
    }

    void init()
    {
        satdump::eventBus->register_handler<dsp::RegisterDSPSampleSourcesEvent>(registerSources);
    }

    static void registerSources(const dsp::RegisterDSPSampleSourcesEvent &evt)
    {
        evt.dsp_sources_registry.insert({BirriaSDRSource::getID(), {BirriaSDRSource::getInstance, BirriaSDRSource::getAvailableSources}});
    }
};

PLUGIN_LOADER(BirriaSDRSupport)
