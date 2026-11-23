#include "core/config.h"
#include "core/params.h"
#include "core/plugin.h"
#include "init.h"
#include "logger.h"
#include "usrp_sdr.h"

#ifdef _WIN32
errno_t setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
#endif

std::shared_ptr<FileSelectWidget> file_select;

// OS-specific USB transport frame settings
static bool use_uhd_defaults = false;
#ifdef __APPLE__
static int recv_frame_size = 8000;
static int num_recv_frames = 1024;
static int send_frame_size = 8000;
static int num_send_frames = 1024;
#elif defined(_WIN32)
static int recv_frame_size = 8000;
static int num_recv_frames = 2048;
static int send_frame_size = 8000;
static int num_send_frames = 2048;
#else
static int recv_frame_size = 8000;
static int num_recv_frames = 1900;
static int send_frame_size = 8000;
static int num_send_frames = 1900;
#endif

class USRPSDRSupport : public satdump::Plugin
{
public:
    static void renderConfig()
    {
        static bool use_default = (file_select->getPath() == "");
        if (ImGui::BeginTable("##usrpsdrsettings", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            // Theme Selection
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Use default UHD firmware images path");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Checkbox("##usrpimagespathcheckbox", &use_default) && use_default)
                file_select->setPath("");
            if (!use_default)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("UHD Images Path");
                ImGui::TableSetColumnIndex(1);
                file_select->draw();
            }
            // USB transport frame settings hidden, unless Advanced Mode is enabled
            bool advanced_mode = getValueOrDefault(satdump::satdump_cfg.main_cfg["user_interface"]["advanced_mode"]["value"], false);
            if (advanced_mode)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Use default UHD transport frame settings");
                ImGui::TableSetColumnIndex(1);
                ImGui::Checkbox("##usrpdefaultframes", &use_uhd_defaults);
                if (!use_uhd_defaults)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("recv_frame_size");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::InputInt("##usrprecvframesize", &recv_frame_size);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("num_recv_frames");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::InputInt("##usrpnumrecvframes", &num_recv_frames);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("send_frame_size");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::InputInt("##usrpsendframesize", &send_frame_size);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("num_send_frames");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::InputInt("##usrpnumsendframes", &num_send_frames);
                }
            }
            ImGui::EndTable();
        }
    }
    static void save()
    {
        setenv("UHD_IMAGES_DIR", file_select->getPath().c_str(), 1);
        if (file_select->getPath() != "")
            satdump::db->set_user("usrp_sdr_support/uhd_images_dir", file_select->getPath());
        if (recv_frame_size < 64)
            recv_frame_size = 64;
        if (num_recv_frames < 1)
            num_recv_frames = 1;
        if (send_frame_size < 64)
            send_frame_size = 64;
        if (num_send_frames < 1)
            num_send_frames = 1;
        satdump::db->set_user("usrp_sdr_support/use_uhd_defaults", use_uhd_defaults ? "1" : "0");
        satdump::db->set_user("usrp_sdr_support/recv_frame_size", std::to_string(recv_frame_size));
        satdump::db->set_user("usrp_sdr_support/num_recv_frames", std::to_string(num_recv_frames));
        satdump::db->set_user("usrp_sdr_support/send_frame_size", std::to_string(send_frame_size));
        satdump::db->set_user("usrp_sdr_support/num_send_frames", std::to_string(num_send_frames));
    }

    std::string getID() { return "usrp_sdr_support"; }

    void init()
    {
        satdump::eventBus->register_handler<dsp::RegisterDSPSampleSourcesEvent>(registerSources);
        satdump::eventBus->register_handler<satdump::config::RegisterPluginConfigHandlersEvent>(registerConfigHandler);

        file_select = std::make_shared<FileSelectWidget>("UHD Images Path", "UHD Images Path", true);
        std::string val = satdump::db->get_user("usrp_sdr_support/uhd_images_dir", "");
        if (val != "")
        {
            file_select->setPath(val);
            setenv("UHD_IMAGES_DIR", file_select->getPath().c_str(), 1);
        }
        use_uhd_defaults = (satdump::db->get_user("usrp_sdr_support/use_uhd_defaults", use_uhd_defaults ? "1" : "0") == "1");
        try
        {
            recv_frame_size = std::stoi(satdump::db->get_user("usrp_sdr_support/recv_frame_size", std::to_string(recv_frame_size)));
        }
        catch (...)
        {
        }
        try
        {
            num_recv_frames = std::stoi(satdump::db->get_user("usrp_sdr_support/num_recv_frames", std::to_string(num_recv_frames)));
        }
        catch (...)
        {
        }
        try
        {
            send_frame_size = std::stoi(satdump::db->get_user("usrp_sdr_support/send_frame_size", std::to_string(send_frame_size)));
        }
        catch (...)
        {
        }
        try
        {
            num_send_frames = std::stoi(satdump::db->get_user("usrp_sdr_support/num_send_frames", std::to_string(num_send_frames)));
        }
        catch (...)
        {
        }
        satdump::db->set_user("usrp_sdr_support/use_uhd_defaults", use_uhd_defaults ? "1" : "0");
        satdump::db->set_user("usrp_sdr_support/recv_frame_size", std::to_string(recv_frame_size));
        satdump::db->set_user("usrp_sdr_support/num_recv_frames", std::to_string(num_recv_frames));
        satdump::db->set_user("usrp_sdr_support/send_frame_size", std::to_string(send_frame_size));
        satdump::db->set_user("usrp_sdr_support/num_send_frames", std::to_string(num_send_frames));
    }

    static void registerSources(const dsp::RegisterDSPSampleSourcesEvent &evt) { evt.dsp_sources_registry.insert({USRPSource::getID(), {USRPSource::getInstance, USRPSource::getAvailableSources}}); }

    static void registerConfigHandler(const satdump::config::RegisterPluginConfigHandlersEvent &evt)
    {
        evt.plugin_config_handlers.push_back({"USRP SDR", USRPSDRSupport::renderConfig, USRPSDRSupport::save});
    }
};

PLUGIN_LOADER(USRPSDRSupport)
