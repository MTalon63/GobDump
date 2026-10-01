#include "image_handler.h"
#include "../vector/shapefile_handler.h"
#include "common/widgets/menuitem_tooltip.h"
#include "common/widgets/very_small_button.h"
#include "core/config.h"
#include "core/plugin.h"
#include "core/style.h"
#include "explorer/explorer.h"
#include "handlers/image/image_filter.h"
#include "handlers/processing_handler.h"
#include "handlers/projection/projection_handler.h"
#include "handlers/vector/addmenu.h"
#include "handlers/vector/shapefile_handler.h"
#include "i18n.h"
#include "image/earth_curvature.h"
#include "image/io.h"
#include "image/meta.h"
#include "image/processing.h"
#include "imgui/imgui.h"
#include "logger.h"
#include "nlohmann/json_utils.h"
#include "products/image/channel_transform.h"
#include "utils/string.h"
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace satdump
{
    namespace handlers
    {
        ImageHandler::ImageHandler()
        {
            handler_tree_icon = u8"\uf7e8";
            setCanSubBeReorgTo(true);

            // Image crop feature
            image_view.cropCallback = [this](int x1, int y1, int x2, int y2)
            {
                if (is_processing)
                {
                    logger->error(_("Cannot crop while processing!")); // TODOREWORK see when adding other functions - maybe a global image lock?
                    return;
                }

                if (x2 < x1)
                    std::swap(x1, x2);
                if (y2 < y1)
                    std::swap(y1, y2);

                auto img_snap = getImage();
                logger->critical("CROPPING %d %d, %d %d, %d %d", x1, y1, x2, y2, img_snap->width(), img_snap->height());

                auto img = img_snap->crop_to(x1, y1, x2, y2);
                auto proj_cfg = image::get_metadata_proj_cfg(*img_snap);
                if (proj_cfg.contains("transform2"))
                {
                    x1 += proj_cfg["transform2"]["bx"].get<double>();
                    y1 += proj_cfg["transform2"]["by"].get<double>();
                    proj_cfg["transform2"]["bx"] = x1;
                    proj_cfg["transform2"]["by"] = y1;
                }
                else
                {
                    proj_cfg["width"] = img.width();
                    proj_cfg["height"] = img.height();
                    proj_cfg["transform2"] = ChannelTransform().init_affine(1, 1, x1, y1);
                }
                image::set_metadata_proj_cfg(img, proj_cfg);
                geocorrect_image = false;

                auto sh = std::make_shared<ImageHandler>(img);
                sh->image_name = image_name + _(" Crop");

                if (removeProjectionInfoFromCrop)
                    image::set_metadata_proj_cfg(img, {});

                if (sendCropToRoot)
                {
                    eventBus->fire_event<explorer::ExplorerAddHandlerEvent>({sh, true});
                }
                else
                {
                    addSubHandler(sh);
                    eventBus->fire_event<explorer::ExplorerSelectHandlerEvent>({sh});
                }
            };

            // Load image filters
            image_filters = getImageFilters();
        }

        ImageHandler::ImageHandler(image::Image img) : ImageHandler::ImageHandler() { setImage(img); }

        ImageHandler::ImageHandler(image::Image img, std::string name) : ImageHandler::ImageHandler(img) { image_name = name; }

        ImageHandler::~ImageHandler()
        {
            ProcessingHandler::~ProcessingHandler();
            if (file_save_thread.joinable())
                file_save_thread.join();
        }

        void ImageHandler::drawMenu()
        {
            bool needs_to_be_disabled = is_processing;

            if (ImGui::CollapsingHeader(_("Image")))
            {
                bool needs_to_update = false;

                if (needs_to_be_disabled)
                    style::beginDisabled();

                if (ImGui::RadioButton(_("Rotate 0°"), rotate_image == 0))
                    needs_to_update = 1, rotate_image = 0;
                if (ImGui::RadioButton(_("Rotate 90°"), rotate_image == 90))
                    needs_to_update = 1, rotate_image = 90;
                if (ImGui::RadioButton(_("Rotate 180°"), rotate_image == 180))
                    needs_to_update = 1, rotate_image = 180;
                if (ImGui::RadioButton(_("Rotate 270°"), rotate_image == 270))
                    needs_to_update = 1, rotate_image = 270;

                bool proj_valid_ui = false, calib_valid_ui = false;
                image::ImgCalibHandler calib_ui;
                {
                    std::lock_guard<std::mutex> l(state_mtx);
                    proj_valid_ui = image_proj_valid;
                    calib_valid_ui = image_calib_valid;
                    calib_ui = image_calib;
                }

                if (proj_valid_ui)
                    needs_to_update |= ImGui::Checkbox(_("Geo Correct"), &geocorrect_image); // TODOREWORK Disable if it can't be?

                if (needs_to_be_disabled)
                    style::endDisabled();

                if (calib_valid_ui)
                {
                    ImGui::Text(_("Calibration Unit %s"), calib_ui.unit.c_str());
                    ImGui::Text(_("Calibration Min %f"), calib_ui.min);
                    ImGui::Text(_("Calibration Max %f"), calib_ui.max);
                }

                if (needs_to_update)
                {
                    if (file_save_thread_running)
                        logger->error(_("Please wait for saving to end first!"));
                    else
                        asyncProcess();
                }
                wasMenuTriggered = needs_to_update;
            }

            if (ImGui::CollapsingHeader(_("Filters"), ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (needs_to_be_disabled)
                    style::beginDisabled();

                if (ImGui::BeginListBox("##filterscombo", {ImGui::GetContentRegionAvail().x, 0}))
                {
                    bool quit = false;

                    for (int i = 0; i < active_filters.size(); i++)
                    {
                        if (quit)
                            break;

                        auto &f = active_filters[i];

                        std::string name = image_filters[f.first].name;

                        ImGui::PushID(i);
                        ImGui::BeginGroup();

                        if (f.second.progress > 0)
                        {
                            auto min = ImGui::GetCursorScreenPos();
                            auto max = ImGui::GetCursorScreenPos() + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::CalcTextSize(name.c_str()).y);
                            max.x = min.x + (max.x - min.x) * f.second.progress;
                            min.y += ImGui::CalcTextSize(name.c_str()).y * 0.9;
                            min.y += 5 * ui_scale;
                            max.y += 5 * ui_scale;
                            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
                        }
                        else if (f.second.progress == -1)
                        {
                            auto min = ImGui::GetCursorScreenPos();
                            auto max = ImGui::GetCursorScreenPos() + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::CalcTextSize(name.c_str()).y);
                            float offset = fmod(ImGui::GetTime() * 100, (max.x - min.x));
                            max.x = offset + min.x + (max.x - min.x) * 0.1;
                            min.x = offset;
                            min.y += ImGui::CalcTextSize(name.c_str()).y * 0.9;
                            min.y += 5 * ui_scale;
                            max.y += 5 * ui_scale;
                            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
                        }

                        // Settings
                        if (!image_filters[f.first].has_menu)
                            style::beginDisabled();
                        if (widgets::VerySmallButton(u8"\uF085"))
                        {
                            image_filter_configurator = image_filters[f.first].configMenuGetter();
                            if (image_filter_configurator)
                            {
                                image_filter_configurator->set(f.second.cfg);
                                image_filter_configurator_set_in = i;
                            }
                        }
                        if (!image_filters[f.first].has_menu)
                            style::endDisabled();

                        if (f.second.cfg.size() && ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", f.second.cfg.dump(4).c_str());

                        ImGui::SameLine();

                        // Delete
                        if (widgets::VerySmallButton(u8"\uF1F8") && i < active_filters.size())
                        {
                            active_filters.erase(active_filters.begin() + i);
                            quit = true;
                            asyncProcess();
                        }

                        ImGui::SameLine();

                        // Up
                        if (i == 0)
                            style::beginDisabled();
                        if (widgets::VerySmallButton(u8"\uF062") && i > 0)
                        {
                            std::swap(active_filters[i], active_filters[i - 1]);
                            asyncProcess();
                        }
                        if (i == 0)
                            style::endDisabled();

                        ImGui::SameLine();

                        // Down
                        if (i == active_filters.size() - 1)
                            style::beginDisabled();
                        if (widgets::VerySmallButton(u8"\uF063") && i + 1 < active_filters.size())
                        {
                            std::swap(active_filters[i], active_filters[i + 1]);
                            asyncProcess();
                        }
                        if (i == active_filters.size() - 1)
                            style::endDisabled();

                        ImGui::SameLine();

                        // Disable/Enable
                        if (widgets::VerySmallButton(active_filters[i].second.enabled ? u8"\uF06E" : u8"\uF070"))
                        {
                            active_filters[i].second.enabled = !active_filters[i].second.enabled;
                            asyncProcess();
                        }

                        ImGui::SameLine();

                        ImGui::Text("%s", name.c_str());

                        ImGui::EndGroup();
                        ImGui::PopID();
                        ImGui::Separator();
                    }
                    ImGui::EndListBox();
                }

                if (needs_to_be_disabled)
                    style::endDisabled();
            }
        }

        void ImageHandler::drawSaveMenu()
        {
            bool needs_to_be_disabled = is_processing || file_save_thread_running;

            if (needs_to_be_disabled)
                style::beginDisabled();

            if (widgets::MenuItemTooltip(u8"\ueb4b", _("Save Image")))
            {
                auto fun = [this]()
                {
                    set_is_processing(true);
                    file_save_thread_running = true;
                    // TODOREWORK!!!!
                    std::string save_type = "png";
                    satdump_cfg.tryAssignValueFromSatDumpGeneral(save_type, "image_format");
                    std::string default_path = satdump_cfg.getValueFromSatDumpDirectories<std::string>("default_image_output_directory");
                    auto img_snap = getImage();
                    std::string saved_at = save_image_dialog(getSaneName(), default_path, _("Save Image"), img_snap.get(), &save_type);
                    if (saved_at == "")
                        logger->info(_("Save cancelled"));
                    else
                        logger->info(_("Saved current image at %s"), saved_at.c_str());
                    file_save_thread_running = false;
                    set_is_processing(false);
                };

                if (file_save_thread.joinable())
                    file_save_thread.join();
                if (file_save_thread_running)
                    logger->error(_("Please wait for processing to end first!"));
                else
                    file_save_thread = std::thread(fun);
            }

            if (needs_to_be_disabled)
                style::endDisabled();
        }

        void ImageHandler::drawMenuBar()
        {
            drawSaveMenu();

            if (enableOverlayMenu && renderVectorOverlayMenu(this))
                asyncProcess();

            /////////////
            // Image Controls
            /////////////

            // Refresh button
            if (widgets::MenuItemTooltip(u8"\uf01e", _("Refresh (Image Only)")))
                asyncProcess();

            // Basic controls
            image_view.zoom_in_next |= widgets::MenuItemTooltip(u8"\ueb81", _("Zoom In"));
            image_view.zoom_out_next |= widgets::MenuItemTooltip(u8"\ueb82", _("Zoom Out"));
            image_view.autoFitNextFrame |= widgets::MenuItemTooltip(u8"\uF69E", _("Fit"));
            image_view.select_crop_next |= widgets::MenuItemTooltip(u8"\uF69D", _("Crop"), NULL, image_view.select_crop_next);

            bool proj_valid_menu = false;
            {
                std::lock_guard<std::mutex> l(state_mtx);
                proj_valid_menu = image_proj_valid;
            }
            if (proj_valid_menu)
            {
                if (rotate_image) // Projs do not work with rotated imagery
                    style::beginDisabled();

                // Show a menu that allows putting this image on an existing or new projection
                if (widgets::BeginMenuTooltip(u8"\uf484", _("Add to projection")))
                {
                    std::vector<std::shared_ptr<Handler>> hs;
                    eventBus->fire_event<explorer::GetAllOfTypeEvent>({"projection_handler", hs});

                    int n = 0;
                    for (auto &h : hs)
                    {
                        std::string id = h->getName() + " (" + std::to_string(++n) + ")" + "##addtoproj";
                        if (ImGui::MenuItem(id.c_str()))
                            h->addSubHandler(std::make_shared<ImageHandler>(*getImage(), getName()), true);
                    }

                    if (n > 0)
                        ImGui::Separator();

                    if (ImGui::MenuItem(_("New Projection")))
                    {
                        auto p = std::make_shared<ProjectionHandler>();
                        p->addSubHandler(std::make_shared<ImageHandler>(*getImage(), getName()), true);
                        eventBus->fire_event<explorer::ExplorerAddHandlerEvent>({p});
                    }

                    ImGui::EndMenu();
                }

                if (rotate_image)
                    style::endDisabled();
            }

            // Render filters menu
            if (widgets::BeginMenuTooltip(_(u8"\uF0C3"), _("Filters"), !is_processing))
            {
                for (auto &filter : image_filters)
                {
                    if (ImGui::MenuItem(filter.second.name.c_str()))
                    {
                        auto menu = filter.second.configMenuGetter();

                        if (menu)
                        {
                            menu->type = filter.first;
                            image_filter_configurator_set_in = -1;
                            image_filter_configurator = menu;
                        }
                        else
                        {
                            active_filters.push_back({filter.first, {{}}});
                            asyncProcess();
                        }
                    }
                }

                ImGui::EndMenu();
            }
        }

        void ImageHandler::drawContents(ImVec2 win_size)
        {
            if (ImGui::BeginChild("ContentChild", win_size, false, ImGuiWindowFlags_NoScrollbar))
            {
                if (imgview_needs_update)
                {
                    {
                        std::lock_guard<std::mutex> l(state_mtx);
                        auto img_snap = getImage();
                        image_view.update(*img_snap);
                    }
                    imgview_needs_update = false;

                    image_view.mouseCallback = [this](float x, float y)
                    {
                        auto img = getImage();
                        ImGui::BeginTooltip();

                        projection::Projection proj_snap;
                        image::ImgCalibHandler calib_snap;
                        std::vector<float> fwd_lut_snap;
                        bool proj_valid_snap = false, calib_valid_snap = false;
                        {
                            std::lock_guard<std::mutex> l(state_mtx);
                            proj_valid_snap = image_proj_valid;
                            if (proj_valid_snap)
                                proj_snap = image_proj;
                            calib_valid_snap = image_calib_valid;
                            if (calib_valid_snap)
                                calib_snap = image_calib;
                            fwd_lut_snap = correct_fwd_lut;
                        }

                        for (int i = 0; i < img->channels(); i++)
                            ImGui::Text(_("Raw %d : %d F %f"), i + 1, img->get(i, x, y), img->getf(i, x, y));

                        if (calib_valid_snap && img->channels() == 1 && x >= 0 && y >= 0 && x < img->width() && y < img->height())
                        {
                            double val = calib_snap.getVal(img->getf(0, x, y));
                            ImGui::Text(_("Unit : %f %s"), val, calib_snap.unit.c_str());
                        }

                        // Handle rotations
                        if (rotate_image)
                        {
                            if (rotate_image == 180)
                            {
                                x = (img->width() - 1) - x;
                                y = (img->height() - 1) - y;
                            }
                            else if (rotate_image == 90)
                            {
                                auto x1 = y;
                                y = (img->width() - 1) - x;
                                x = x1;
                            }
                            else if (rotate_image == 270)
                            {
                                auto x1 = (img->height() - 1) - y;
                                y = x;
                                x = x1;
                            }
                        }

                        if (fwd_lut_snap.size() > 0)
                        {
                            if (x >= 0 && x < fwd_lut_snap.size())
                                x = fwd_lut_snap[x];
                            else
                            {
                                ImGui::Text(_("Error in geo-correction!"));
                                return;
                            }
                        }

                        if (proj_valid_snap)
                        {
                            geodetic::geodetic_coords_t pos;
                            if (proj_snap.inverse(x, y, pos))
                            {
                                ImGui::Text(_("Lat : Invalid!"));
                                ImGui::Text(_("Lon : Invalid!"));
                            }
                            else
                            {
                                ImGui::Text(_("Lat : %f"), pos.lat);
                                ImGui::Text(_("Lon : %f"), pos.lon);
                            }
                        }
                        additionalMouseCallback(x, y);
                        ImGui::EndTooltip();
                    };
                }

                image_view.draw({ImGui::GetWindowSize().x, ImGui::GetWindowSize().y + 14 * ui_scale});

                ImGui::EndChild();
            }

            // Render image filter config
            if (image_filter_configurator)
            {
                ImGui::OpenPopup(_("Filter Config"));
                if (ImGui::BeginPopupModal(_("Filter Config"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    image_filter_configurator->draw();

                    if (ImGui::Button(_("Apply")))
                    {
                        if (image_filter_configurator_set_in == -1)
                            active_filters.push_back({image_filter_configurator->type, {image_filter_configurator->get()}});
                        else if (image_filter_configurator_set_in < active_filters.size())
                            active_filters[image_filter_configurator_set_in].second.cfg = image_filter_configurator->get();
                        image_filter_configurator.reset();
                        asyncProcess();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(_("Cancel")))
                        image_filter_configurator.reset();

                    ImGui::EndPopup();
                }
            }
        }

        void ImageHandler::setConfig(nlohmann::json p)
        {
            rotate_image = getValueOrDefault(p["rotate"], rotate_image);
            geocorrect_image = getValueOrDefault(p["geocorrect"], geocorrect_image);

            try
            {
                if (p.contains("filters"))
                    active_filters = p["filters"];
                else
                    active_filters.clear();
            }
            catch (std::exception &e)
            {
                logger->error("Error parsing filters : %s", e.what());
            }
        }

        nlohmann::json ImageHandler::getConfig()
        {
            nlohmann::json p;
            p["rotate"] = rotate_image;
            p["geocorrect"] = geocorrect_image;
            p["filters"] = active_filters;
            return p;
        }

        void ImageHandler::setImage(image::Image &img) // TODOREWORK
        {
            {
                std::lock_guard<std::mutex> l(state_mtx);
                auto new_img = std::make_shared<image::Image>(img);
                image::set_metadata(*new_img, {});
                std::atomic_store(&image, new_img);
            }
            process();
        }

        std::string ImageHandler::getSaneName()
        {
            std::string img_name = image_name;
            replaceAllStr(img_name, " ", "_");
            replaceAllStr(img_name, "/", "_");
            replaceAllStr(img_name, "\\", "_");
            return img_name;
        }

        // TODOREWORK?
        bool ImageHandler::saveResult(std::string directory)
        {
            auto img_snap = getImage();
            image::save_img_safe(*img_snap, directory + "/" + getSaneName());
            return img_snap->size();
        }

        void ImageHandler::do_process()
        {
            bool image_needs_processing = rotate_image | geocorrect_image | active_filters.size();

            correct_fwd_lut.clear();
            correct_rev_lut.clear();

            std::shared_ptr<image::Image> work;

            if (image_needs_processing)
            {
                work = std::make_shared<image::Image>(*std::atomic_load(&image));

                try
                {
                    for (auto &f : active_filters)
                    {
                        if (f.second.enabled)
                        {
                            f.second.progress = -1;

                            if (image_filters.count(f.first))
                            {
                                logger->info("Applying filter : " + f.first);
                                image_filters[f.first].perform(*work, f.second.cfg, &f.second.progress);
                            }
                            else
                            {
                                logger->error("Could not find image filter " + f.first + "!");
                            }

                            f.second.progress = 1;
                        }
                    }

                    if (geocorrect_image)
                    { // TODOREWORK handle disabling projs, etc
                        bool success = false;
                        work = std::make_shared<image::Image>(image::earth_curvature::perform_geometric_correction(*work, success, &correct_rev_lut, &correct_fwd_lut));
                        if (!success)
                        {
                            logger->error(_("Failed Geo-Correcting image!"));
                            correct_fwd_lut.clear();
                            correct_rev_lut.clear();
                        }
                    }
                }
                catch (std::exception &e)
                {
                    logger->error(_("Error processing image! %s"), e.what());
                }
            }

            for (auto &f : active_filters)
                f.second.progress = 0;

            int pre_proj_w = work && work->size() ? work->width() : std::atomic_load(&image)->width();
            int pre_proj_h = work && work->size() ? work->height() : std::atomic_load(&image)->height();

            // Special case for rotations
            try
            {
                if (rotate_image)
                    image::rotate(*work, rotate_image);
            }
            catch (std::exception &e)
            {
                logger->error("Error processing image! %s", e.what());
            }

            ////////////////////////
            auto overlay_handlers = getAllSubHandlers();
            bool image_has_overlays = false;

            for (auto &h : overlay_handlers)
                if (h->getID() == "shapefile_handler")
                    image_has_overlays = true;

            if (image_has_overlays)
            {
                if (!work || work->size() == 0)
                    work = std::make_shared<image::Image>(*std::atomic_load(&image));

                nlohmann::json cfg = image::get_metadata_proj_cfg(*work);
                cfg["width"] = pre_proj_w;
                cfg["height"] = pre_proj_h;
                std::unique_ptr<projection::Projection> p = std::make_unique<projection::Projection>();
                *p = cfg;
                p->init(1, 0);

                int rotate_image_l = rotate_image;

                auto pfunc = [&p, this, rotate_image_l, pre_proj_w, pre_proj_h](double lat, double lon, double h, double w) mutable -> std::pair<double, double>
                {
                    double x, y, x2, y2;
                    if (p->forward(geodetic::geodetic_coords_t(lat, lon, 0, false), x, y) || x < 0 || x >= pre_proj_w || y < 0 || y >= pre_proj_h)
                        x2 = -1, y2 = -1;
                    else if (rotate_image_l == 0)
                        x2 = x, y2 = y;
                    else if (rotate_image_l == 90)
                        x2 = (w - 1) - y, y2 = x;
                    else if (rotate_image_l == 180)
                        x2 = (w - 1) - x, y2 = (h - 1) - y;
                    else if (rotate_image_l == 270)
                        x2 = y, y2 = (h - 1) - x;
                    else
                        x2 = -1, y2 = -1;

                    if (x2 < 0 || x2 >= w || y2 < 0 || y2 >= h)
                        return {-1, -1};
                    else
                        return {x2, y2};
                };

                for (int i = overlay_handlers.size() - 1; i >= 0; i--)
                {
                    auto &h = overlay_handlers[i];
                    if (h->getID() == "shapefile_handler")
                    {
                        ShapefileHandler *sh_h = (ShapefileHandler *)h.get();
                        logger->critical("Drawing OVERLAY!");
                        sh_h->draw_to_image(*work, pfunc);
                    }
                }
            }

            ////////////////////////

            std::atomic_store(&curr_image, work);

            // Update ImgView
            imgview_needs_update = true;

            projection::Projection new_proj;
            bool new_proj_valid = false;
            if (image::has_metadata_proj_cfg(*std::atomic_load(&image)))
            {
                new_proj = image::get_metadata_proj_cfg(*std::atomic_load(&image));
                new_proj_valid = new_proj.init(0, 1);
            }

            image::ImgCalibHandler new_calib;
            bool new_calib_valid = false;
            if (image::has_metadata_calib_cfg(*std::atomic_load(&image)))
            {
                new_calib = image::get_metadata_calib_cfg(*std::atomic_load(&image));
                new_calib_valid = true;
            }

            {
                std::lock_guard<std::mutex> l(state_mtx);
                image_proj = std::move(new_proj);
                image_proj_valid = new_proj_valid;
                image_calib = new_calib;
                image_calib_valid = new_calib_valid;
            }
        }
    } // namespace handlers
} // namespace satdump
