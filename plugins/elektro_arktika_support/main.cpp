#include "cli/ggak_ingestor.h"
#include "cli/ggak_merger.h"
#include "core/cli/cli.h"
#include "core/plugin.h"
#include "elektro_arktika/ggak/module_ggak_to_mqtt.h"
#include "logger.h"

#include "elektro_arktika/instruments/msugs/module_msugs.h"
#include "elektro_arktika/instruments/msugs/msugs_proj.h"
#include "elektro_arktika/lrit/module_elektro_lrit_data_decoder.h"
#include "projection/raytrace/satellite_raytracer.h"

class ElektroArktikaSupport : public satdump::Plugin
{
public:
    std::string getID() { return "elektro_arktika_support"; }

    void init()
    {
        satdump::eventBus->register_handler<satdump::pipeline::RegisterModulesEvent>(registerPluginsHandler);
        satdump::eventBus->register_handler<satdump::projection::RequestSatelliteRaytracerEvent>(provideSatProjHandler);

        satdump::eventBus->register_handler<satdump::cli::RegisterSubcommandEvent>(registerCliCommands);
    }

    static void provideSatProjHandler(const satdump::projection::RequestSatelliteRaytracerEvent &evt)
    {
        if (evt.id == "normal_single_xy_line")
            evt.r.push_back(std::make_shared<satdump::projection::NormalLineXYSatProj>(evt.cfg));
    }

    static void registerCliCommands(const satdump::cli::RegisterSubcommandEvent &evt)
    {
        evt.cmd_handlers.push_back(std::make_shared<satdump::GGAKMergerCmdHandler>());
        evt.cmd_handlers.push_back(std::make_shared<satdump::GGAKIngestorCmdHandler>());
    }

    static void registerPluginsHandler(const satdump::pipeline::RegisterModulesEvent &evt)
    {
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro_arktika::msugs::MSUGSDecoderModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro::lrit::ELEKTROLRITDataDecoderModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro_arktika::ggak::GGAKToMQTTModule);
    }
};

PLUGIN_LOADER(ElektroArktikaSupport)