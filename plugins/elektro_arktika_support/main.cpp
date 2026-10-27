#include "cli/ggak_ingestor.h"
#include "cli/ggak_merger.h"
#include "cli/rdas_align.h"
#include "core/cli/cli.h"
#include "core/plugin.h"
#include "elektro_arktika/ggak/module_ggak_to_mqtt.h"
#include "logger.h"

#include "elektro_arktika/instruments/ggak/ggak_product.h"
#include "elektro_arktika/instruments/ggak/ggak_product_handler.h"
#include "elektro_arktika/instruments/ggak/module_ggak.h"
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
        satdump::eventBus->register_handler<satdump::products::RegisterProductsEvent>(registerProductsHandler);
        satdump::eventBus->register_handler<satdump::handlers::RequestProductHandlerEvent>(registerProductHandlerHandler);

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
        evt.cmd_handlers.push_back(std::make_shared<satdump::RDASAlignCmdHandler>());
    }

    static void registerProductsHandler(const satdump::products::RegisterProductsEvent &evt)
    {
        evt.product_loaders.emplace("ggak", satdump::products::RegisteredProduct{
            [](std::string file) -> std::shared_ptr<satdump::products::Product> {
                auto product = std::make_shared<elektro_arktika::ggak::GGAKProduct>();
                product->load(file);
                return product;
            }});
    }

    static void registerProductHandlerHandler(const satdump::handlers::RequestProductHandlerEvent &evt)
    {
        if (evt.product->type == "ggak")
            evt.handler = std::make_shared<elektro_arktika::ggak::GGAKProductHandler>(evt.product, evt.dataset_mode);
    }

    static void registerPluginsHandler(const satdump::pipeline::RegisterModulesEvent &evt)
    {
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro_arktika::msugs::MSUGSDecoderModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro::lrit::ELEKTROLRITDataDecoderModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro_arktika::ggak::GGAKToMQTTModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, elektro_arktika::ggak::GGAKDecoderModule);
    }
};

PLUGIN_LOADER(ElektroArktikaSupport)