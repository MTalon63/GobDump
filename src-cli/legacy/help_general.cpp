#include "help_general.h"
#include "init.h"
#include "logger.h"
#include "satdump_vars.h"

void help_general()
{
    logger->error("");
    logger->error("Visit: www.satdump.org");
    logger->error("");
    logger->info("Many usecases of GobDump CLI are cover at the following link");
    logger->debug("www.satdump.org/posts/basic-usage/#cli-this-part-was-made-by-aang23");
    logger->info("");
    logger->info("This is GobDump v" + (std::string)satdump::SATDUMP_VERSION);
    logger->info("Live processing");
    logger->debug("	- Usage: gobdump live + parameters");
    logger->debug("	- info: use 'gobdump live' for more information on 'live' usage and parameters");
    logger->info("record processing");
    logger->debug("	- Usage: gobdump record + parameters");
    logger->debug("	- info: use 'gobdump record' for more information on 'record' usage and parameters");
    logger->info("autotrack feature");
    logger->debug("	- Usage: gobdump autotrack + parameters");
    logger->debug("	- info: use 'gobdump autotrack' for more information on 'autotrack' usage and parameters");
    logger->info("GobDump GUI version");
    logger->debug("	- Usage: gobdump-ui");
    logger->debug("	- info: GUI version of GobDump");
    logger->info("SDR probe");
    logger->debug("	- Usage: gobdump sdr_probe");
    logger->debug("	- info: Return a list of local and remote receivers ");
    logger->info("General help");
    logger->debug("	- Usage: gobdump help or -h");
    logger->debug("	- info: Display the general help function");
    logger->info("version of GobDump");
    logger->debug("	- Usage: gobdump version");
    logger->debug("	- info: Display the current version of GobDump. Also visible above");
    exit(0);
}
