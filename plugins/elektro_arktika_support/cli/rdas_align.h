#pragma once

#include "core/cli/subcommand.h"

#include <string>

namespace satdump
{
    /// Re-run RDAS / MSU-GS layer alignment over an existing output directory.
    class RDASAlignCmdHandler : public CmdHandler
    {
    private:
        std::string directory;
        std::string satellite = "L2";
        bool recalibrate = false;

    public:
        RDASAlignCmdHandler() : CmdHandler("rdas_align") {}

        void reg(CLI::App *app);
        void run(CLI::App *app, CLI::App *subcom, bool is_gui);
    };
} // namespace satdump
