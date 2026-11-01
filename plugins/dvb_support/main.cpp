#include "common/cpu_features.h"
#include "core/plugin.h"
#include "logger.h"

#include "dvbs/module_dvbs_demod.h"
#include "dvbs2/module_dvbs2_demod.h"
#include "dvbs2/module_s2_ts_extractor.h"

// Only the DVB-S2 sources are built with AVX2, so only they have to be held back.
static bool dvbs2_isa_available()
{
#ifdef DVB_HAS_AVX2
    return cpu_features::get_cpu_features().CPU_X86_AVX2;
#else
    return true;
#endif
}

class DVBSupport : public satdump::Plugin
{
public:
    std::string getID() { return "dvb_support"; }

    void init()
    {
#ifdef DVB_HAS_SSE41
        if (!cpu_features::get_cpu_features().CPU_X86_SSE41)
        {
            logger->error("CPU Does not support SSE4_1. Extension plugin NOT loading!");
            return;
        }
#endif
#ifdef DVB_HAS_NEON
        if (!cpu_features::get_cpu_features().CPU_ARM_NEON)
        {
            logger->error("CPU Does not support NEON. Extension plugin NOT loading!");
            return;
        }
#endif
        if (!dvbs2_isa_available())
            logger->error("CPU Does not support AVX2. DVB-S2 modules NOT loading! (DVB-S is unaffected)");

        satdump::eventBus->register_handler<satdump::pipeline::RegisterModulesEvent>(registerPluginsHandler);
    }

    static void registerPluginsHandler(const satdump::pipeline::RegisterModulesEvent &evt)
    {
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, satdump::pipeline::dvb::DVBSDemodModule);

        if (!dvbs2_isa_available())
            return;

        REGISTER_MODULE_EXTERNAL(evt.modules_registry, satdump::pipeline::dvb::DVBS2DemodModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, dvbs2::S2TStoTCPModule);
    }
};

PLUGIN_LOADER(DVBSupport)