#pragma once

#include "common/tile_map/map.h"
#include "dll_export.h"
#include "explorer/explorer.h"
#include "libs/ctpl/ctpl_stl.h"
#include "recorder/recorder.h"
#include <string>

namespace satdump
{
    SATDUMP_DLL2 extern bool update_ui;
    // Reference to a deliberately leaked pool - see main_ui.cpp for why it must not be destroyed
    SATDUMP_DLL2 extern ctpl::thread_pool &ui_thread_pool;

    SATDUMP_DLL2 extern std::shared_ptr<explorer::ExplorerApplication> explorer_app;

    void initMainUI();
    void exitMainUI();
    void renderMainUI();

    // TODOREWORK move into another namespace?
    struct SetIsProcessingEvent
    {
    };

    struct SetIsDoneProcessingEvent
    {
    };

    struct ShowProcesingEvent
    {
    };

    /**
     * @brief Fired every frame on the UI thread, GL context current. Lets
     * plugins defer GL work to the UI thread.
     */
    struct UIRenderFrameEvent
    {
    };

    struct AddRecorderEvent // TODOREWORK Temporary!?
    {
    };

    struct TryOpenFileInMainExplorerEvent
    {
        std::string path;
    };
} // namespace satdump
