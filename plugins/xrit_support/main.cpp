#include "core/plugin.h"
#include "main_ui.h"
#include "xrit/elektro/msugs_xrit_calibrator.h"
#include "xrit/generic_xrit_calibrator.h"
#include "xrit/processor/xrit_channel_processor.h"

class XRITSupport : public satdump::Plugin
{
public:
    std::string getID() { return "xrit_support"; }

    void init()
    {
        satdump::eventBus->register_handler<satdump::products::RequestImageCalibratorEvent>(provideImageCalibratorHandler);

        // Drain deferred GL texture deletions every frame, on the UI thread
        satdump::eventBus->register_handler<satdump::UIRenderFrameEvent>(
            [](const satdump::UIRenderFrameEvent &) { satdump::xrit::XRITChannelProcessor::drainPendingTextureDeletes(); });
    }

    static void provideImageCalibratorHandler(const satdump::products::RequestImageCalibratorEvent &evt)
    {
        if (evt.id == "generic_xrit")
            evt.calibrators.push_back(std::make_shared<satdump::xrit::GenericxRITCalibrator>(evt.products, evt.calib));
        else if (evt.id == "msugs_xrit")
            evt.calibrators.push_back(std::make_shared<satdump::xrit::MSUGSXritCalibrator>(evt.products, evt.calib));
    }
};

PLUGIN_LOADER(XRITSupport)