#include "debug.hpp"
#include "output.hpp"
#include "core.hpp"
#include "output-layout.hpp"
#include "workspace-manager.hpp"
#include "signal-definitions.hpp"
#include "seat/input-manager.hpp"
#include "seat/input-inhibit.hpp"
#include "util.hpp"
#include "view-transform.hpp"
#include <xf86drmMode.h>

extern "C"
{
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
}

static wl_output_transform get_transform_from_string(std::string transform)
{
    if (transform == "normal") return WL_OUTPUT_TRANSFORM_NORMAL;
    else if (transform == "90") return WL_OUTPUT_TRANSFORM_90;
    else if (transform == "180") return WL_OUTPUT_TRANSFORM_180;
    else if (transform == "270") return WL_OUTPUT_TRANSFORM_270;
    else if (transform == "flipped") return WL_OUTPUT_TRANSFORM_FLIPPED;
    else if (transform == "180_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_180;
    else if (transform == "90_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_90;
    else if (transform == "270_flipped") return WL_OUTPUT_TRANSFORM_FLIPPED_270;

    log_error ("Bad output transform in config: %s", transform.c_str());
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

std::pair<wlr_output_mode, bool> parse_output_mode(std::string modeline)
{
    wlr_output_mode mode;
    /* If mode refresh is invalid, then it will be autodetected */
    mode.refresh = 0;
    int read = std::sscanf(modeline.c_str(), "%d x %d @ %d", &mode.width, &mode.height, &mode.refresh);

    if (mode.refresh < 1000)
        mode.refresh *= 1000;

    if (read < 2 || mode.width <= 0 || mode.height <= 0 || mode.refresh < 0)
        return {mode, false};

    return {mode, true};
}

std::pair<wf_point, bool> parse_output_layout(std::string layout)
{
    wf_point pos;
    int read;

    if (layout.find("@") != layout.npos)
        read = std::sscanf(layout.c_str(), "%d @ %d", &pos.x, &pos.y);
    else
        read = std::sscanf(layout.c_str(), "%d , %d", &pos.x, &pos.y);

    if (read < 2)
    {
        log_error("Detected invalid layout in config: %s", layout.c_str());
        return {{0, 0}, false};
    }

    return {pos, true};
}

wlr_output_mode *find_matching_mode(wlr_output *output, const wlr_output_mode& reference)
{
    wlr_output_mode *mode;
    wlr_output_mode *best = NULL;
    wl_list_for_each(mode, &output->modes, link)
    {
        if (mode->width == reference.width && mode->height == reference.height)
        {
            if (mode->refresh == reference.refresh)
                return mode;

            if (!best || best->refresh < mode->refresh)
                best = mode;
        }
    }

    return best;
}

// from rootston
static bool parse_modeline(const char *modeline, drmModeModeInfo &mode)
{
    char hsync[16];
    char vsync[16];
    float fclock;

    mode.type = DRM_MODE_TYPE_USERDEF;

    if (sscanf(modeline, "%f %hd %hd %hd %hd %hd %hd %hd %hd %15s %15s",
               &fclock, &mode.hdisplay, &mode.hsync_start, &mode.hsync_end,
               &mode.htotal, &mode.vdisplay, &mode.vsync_start, &mode.vsync_end,
               &mode.vtotal, hsync, vsync) != 11) {
        return false;
    }

    mode.clock = fclock * 1000;
    mode.vrefresh = mode.clock * 1000.0 * 1000.0 / mode.htotal / mode.vtotal;
    if (strcasecmp(hsync, "+hsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_PHSYNC;
    } else if (strcasecmp(hsync, "-hsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_NHSYNC;
    } else {
        return false;
    }

    if (strcasecmp(vsync, "+vsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_PVSYNC;
    } else if (strcasecmp(vsync, "-vsync") == 0) {
        mode.flags |= DRM_MODE_FLAG_NVSYNC;
    } else {
        return false;
    }

    snprintf(mode.name, sizeof(mode.name), "%dx%d@%d",
             mode.hdisplay, mode.vdisplay, mode.vrefresh / 1000);

    return true;
}

namespace wf
{
    void transfer_views(wayfire_output *from, wayfire_output *to)
    {
        /* first move each desktop view(e.g windows) to another output */
        std::vector<wayfire_view> views;
        from->workspace->for_each_view_reverse([&views] (wayfire_view view) { views.push_back(view); },
            WF_MIDDLE_LAYERS | WF_LAYER_MINIMIZED);

        for (auto& view : views)
            from->detach_view(view);

        for (auto& view : views)
        {
            to->attach_view(view);
            to->focus_view(view);

            if (view->maximized)
                view->maximize_request(true);

            if (view->fullscreen)
                view->fullscreen_request(to, true);
        }

        /* just remove all other views - backgrounds, panels, etc.
         * desktop views have been removed by the previous cycle */
        from->workspace->for_each_view([] (wayfire_view view) {
            view->close();
            view->set_output(nullptr);
        }, WF_ALL_LAYERS);
        /* A note: at this point, some views might already have been deleted */
    }

    constexpr wf_point output_state_t::default_position;
    bool output_state_t::operator == (const output_state_t& other) const
    {
        if (source == OUTPUT_IMAGE_SOURCE_NONE)
            return other.source == OUTPUT_IMAGE_SOURCE_NONE;

        bool eq = true;

        eq &= source == other.source;
        eq &= position == other.position;
        eq &= (mode.width == other.mode.width);
        eq &= (mode.height == other.mode.height);
        eq &= (mode.refresh == other.mode.refresh);
        eq &= (transform == other.transform);
        eq &= (scale == other.scale);

        return eq;
    }

    /** Represents a single output in the output layout */
    struct output_layout_output_t
    {
        wlr_output *handle;
        output_state_t current_state;

        std::unique_ptr<wayfire_output> output;
        wl_listener_wrapper on_destroy;
        wf_option mode_opt, position_opt, scale_opt, transform_opt;
        const std::string default_value = "default";

        output_layout_output_t(wlr_output *handle)
        {
            this->handle = handle;
            on_destroy.connect(&handle->events.destroy);

            auto output_section = core->config->get_section(handle->name);
            mode_opt      = output_section->get_option("mode", default_value);
            position_opt  = output_section->get_option("layout", default_value);
            scale_opt     = output_section->get_option("scale", "1");
            transform_opt = output_section->get_option("transform", "normal");
        }

        wlr_output_mode select_default_mode()
        {
            wlr_output_mode *mode;
            wl_list_for_each(mode, &handle->modes, link)
            {
                if (mode->flags & WL_OUTPUT_MODE_PREFERRED)
                    return *mode;
            }

            /* Couldn't find a preferred mode. Just return the last, which is
             * usually also the "largest" */
            wl_list_for_each_reverse(mode, &handle->modes, link)
                return *mode;

            /* Finally, if there isn't any mode (for ex. wayland backend),
             * try a default resolution */
            return wlr_output_mode {
                .width = 1280,
                .height = 720,
                .refresh = 60000
            };
        }

        /* Returns true if mode setting for the given output can succeed */
        bool is_mode_supported(wlr_output_mode query)
        {
            /* DRM doesn't support setting a custom mode, so any supported mode
             * must be found in the mode list */
            if (wlr_output_is_drm(handle))
            {
                wlr_output_mode *mode;
                wl_list_for_each(mode, &handle->modes, link)
                {
                    if (mode->width == query.width && mode->height == query.height)
                        return true;
                }

                return false;
            }

            /* X11 and Wayland backends support setting custom modes */
            return true;
        }

        output_state_t load_state_from_config()
        {
            output_state_t state;

            state.position = output_state_t::default_position;
            if (position_opt->as_string() != default_value)
            {
                auto value = parse_output_layout(position_opt->as_string());
                if (value.second)
                    state.position = value.first;
            }

            /* Make sure we can use custom modes that are
             * specified in the config */
            refresh_custom_modes();

            if (mode_opt->as_string() == "off")
            {
                state.source = OUTPUT_IMAGE_SOURCE_NONE;
                return state;
            }
            else
            {
                state.source = OUTPUT_IMAGE_SOURCE_SELF;
                state.mode.width = -1;
                if (mode_opt->as_string() != default_value &&
                    mode_opt->as_string() != "auto")
                {
                    auto value = parse_output_mode(mode_opt->as_string());
                    if (value.second)
                    {
                        if (is_mode_supported(value.first)) {
                            state.mode = value.first;
                        } else {
                            log_error("Output mode %s for output %s is not "
                                " supported, try adding a custom mode.",
                                mode_opt->as_string().c_str(), handle->name);
                        }
                    }
                }

                /* Nothing usable in config, try to pick a default mode */
                if (state.mode.width < 0)
                    state.mode = select_default_mode();
            }

            state.scale = scale_opt->as_double();
            if (state.scale <= 0)
            {
                log_error("Invalid scale for %s in config: %s", handle->name,
                    scale_opt->as_string().c_str());
                state.scale = 1;
            }

            state.transform = get_transform_from_string(transform_opt->as_string());
            return state;
        }

        void ensure_wayfire_output()
        {
            if (this->output)
                return;

            this->output = std::make_unique<wayfire_output> (handle, core->config);
            auto wo = output.get();

            /* Focus the first output, but do not change the focus on subsequently
             * added outputs */
            if (core->get_active_output() == nullptr)
                core->focus_output(wo);

            output_added_signal data;
            data.output = wo;
            core->output_layout->emit_signal("output-added", &data);
        }

        void destroy_wayfire_output()
        {
            if (!this->output)
                return;

            log_info("disable output: %s", output->handle->name);
            auto wo = output.get();
            if (core->get_active_output() == wo)
                core->focus_output(core->output_layout->get_next_output(wo));

            transfer_views(wo, core->get_active_output());

            output_removed_signal data;
            data.output = wo;
            core->output_layout->emit_signal("output-removed", &data);

            this->output = nullptr;
        }

        std::set<std::string> added_custom_modes;
        void add_custom_mode(std::string modeline)
        {
            if (added_custom_modes.count(modeline))
                return;

            added_custom_modes.insert(modeline);
            drmModeModeInfo *mode = new drmModeModeInfo;
            if (!parse_modeline(modeline.c_str(), *mode))
            {
                log_error("invalid modeline %s in config file", modeline.c_str());
                return;
            }

            log_debug("output %s: adding custom mode %s", handle->name, mode->name);
            if (wlr_output_is_drm(handle))
                wlr_drm_connector_add_mode(handle, mode);
        }

        void refresh_custom_modes()
        {
            static const std::string custom_mode_prefix = "custom_mode";
            auto section = core->config->get_section(handle->name);
            for (auto& opt : section->options)
            {
                if (custom_mode_prefix == opt->name.substr(0, custom_mode_prefix.length()))
                    add_custom_mode(opt->as_string());
            }
        }

        /** Check whether the given state can be applied */
        bool test_state(const output_state_t& state)
        {
            if (state.source == OUTPUT_IMAGE_SOURCE_NONE)
                return true;

            /* XXX: are there more things to check? */
            refresh_custom_modes();
            return is_mode_supported(state.mode);
        }

        /** Change the output mode */
        bool apply_mode(const wlr_output_mode& mode)
        {
            if (handle->current_mode)
            {
                /* Do not modeset if nothing changed */
                if (handle->current_mode->width == mode.width &&
                    handle->current_mode->height == mode.height &&
                    handle->current_mode->refresh == mode.refresh)
                    return false;
            }

            refresh_custom_modes();
            auto built_in = find_matching_mode(handle, mode);
            if (built_in)
            {
                wlr_output_set_mode(handle, built_in);
            } else
            {
                log_info("Couldn't find matching mode %dx%d@%f for output %s."
                    "Trying to use custom mode (might not work).",
                    mode.width, mode.height, mode.refresh / 1000.0, handle->name);
                wlr_output_set_custom_mode(handle, mode.width, mode.height,
                    mode.refresh / 1000.0);
            }

            return true;
        }

        /** Apply the given state to the output, ignoring position.
         *
         * This won't have any effect if the output state can't be applied,
         * i.e if test_state(state) == false */
        void apply_state(const output_state_t& state)
        {
            if (!test_state(state))
                return;

            this->current_state = state;
            if (state.source & OUTPUT_IMAGE_SOURCE_NONE)
            {
                wlr_output_enable(handle, false);
                if (state.source == OUTPUT_IMAGE_SOURCE_NONE)
                {
                    destroy_wayfire_output();
                    return;
                }
            }
            else
            {
                wlr_output_enable(handle, true);
            }

            bool changed = apply_mode(state.mode);
            if (handle->transform != state.transform)
            {
                changed = true;
                wlr_output_set_transform(handle, state.transform);
            }

            if (handle->scale != state.scale)
            {
                changed = true;
                wlr_output_set_scale(handle, state.scale);
            }

            ensure_wayfire_output();
            if (output && changed)
                output->emit_signal("output-configuration-changed", nullptr);
        }

    };

    class output_layout_t::impl
    {
        std::map<wlr_output*, std::unique_ptr<output_layout_output_t>> outputs;

        wlr_output_layout *output_layout;
        wl_listener_wrapper on_new_output;

        signal_callback_t on_config_reload;

        public:
        impl(wlr_backend *backend)
        {
            on_new_output.set_callback([=] (void *data) { add_output((wlr_output*) data); });
            on_new_output.connect(&backend->events.new_output);

            output_layout = wlr_output_layout_create();

            on_config_reload = [=] (void*) { reconfigure_from_config(); };
            core->connect_signal("reload-config", &on_config_reload);
        }

        ~impl()
        {
            core->disconnect_signal("reload-config", &on_config_reload);
        }

        void add_output(wlr_output *output)
        {
            log_info("new output: %s", output->name);

            auto lo = new output_layout_output_t(output);
            outputs[output] = std::unique_ptr<output_layout_output_t>(lo);
            lo->on_destroy.set_callback([output, this] (void*) { remove_output(output); });

            reconfigure_from_config();
        }

        void remove_output(wlr_output *output)
        {
            log_info("remove output: %s", output->name);

            outputs[output]->destroy_wayfire_output();
            outputs.erase(output);

            /* we have no outputs, simply quit */
            if (outputs.empty())
                std::exit(0);
        }

        /* Get the current configuration of all outputs */
        output_configuration_t get_current_configuration()
        {
            output_configuration_t configuration;
            for (auto& entry : this->outputs)
                configuration[entry.first] = entry.second->current_state;

            return configuration;
        }

        /** Load config from file, test and apply */
        void reconfigure_from_config()
        {
            /* Load from config file */
            output_configuration_t configuration;
            for (auto& entry : this->outputs)
                configuration[entry.first] = entry.second->load_state_from_config();

            if (configuration == get_current_configuration())
                return;

            if (test_configuration(configuration))
                apply_configuration(configuration);
        }

        /** Check whether the given configuration can be applied */
        bool test_configuration(const output_configuration_t& config)
        {
            if (config.size() != this->outputs.size())
                return false;

            bool ok = true;
            for (auto& entry : config)
            {
                if (this->outputs.count(entry.first) == 0)
                    return false;

                ok &= this->outputs[entry.first]->test_state(entry.second);
            }

            /* TODO: reject overlapping outputs */
            /* TODO: possibly reject disjoint outputs? */
            return ok;
        }

        /** Apply the given configuration. Config MUST be a valid configuration */
        void apply_configuration(const output_configuration_t& config)
        {
            for (auto& entry : config)
            {
                auto& handle = entry.first;
                auto& state = entry.second;
                auto& lo = this->outputs[handle];

                wlr_output_layout_remove(output_layout, handle);
                if (state.source & OUTPUT_IMAGE_SOURCE_SELF)
                {
                    if (entry.second.position != output_state_t::default_position) {
                        wlr_output_layout_add(output_layout, handle,
                            state.position.x, state.position.y);
                    } else {
                        wlr_output_layout_add_auto(output_layout, handle);
                    }
                }

                lo->apply_state(state);
            }

            core->output_layout->emit_signal("configuration-updated", nullptr);
        }

        /* Public API functions */
        wlr_output_layout *get_handle() { return output_layout; }
        size_t get_num_outputs() { return get_outputs().size(); }

        wayfire_output *find_output(wlr_output *output)
        {
            if (outputs.count(output))
                return outputs[output]->output.get();

            return nullptr;
        }

        wayfire_output *find_output(std::string name)
        {
            for (auto& entry : outputs)
            {
                if (entry.first->name == name)
                    return entry.second->output.get();
            }

            return nullptr;
        }

        std::vector<wayfire_output*> get_outputs()
        {
            std::vector<wayfire_output*> result;
            for (auto& entry : outputs)
            {
                if (entry.second->current_state.source & OUTPUT_IMAGE_SOURCE_SELF)
                    result.push_back(entry.second->output.get());
            }

            return result;
        }

        wayfire_output *get_next_output(wayfire_output *output)
        {
            auto os = get_outputs();
            if (os.empty())
                return nullptr;

            auto it = std::find(os.begin(), os.end(), output);
            if (it == os.end()) {
                return os.empty() ? nullptr : os[0];
            } else {
                ++it;
                return *it;
            }
        }

        wayfire_output *get_output_coords_at(int x, int y, int& rx, int& ry)
        {
            double lx = x, ly = y;
            wlr_output_layout_closest_point(output_layout, NULL, lx, ly, &lx, &ly);
            auto handle = wlr_output_layout_output_at(output_layout, lx, ly);
            assert(handle);

            rx = lx;
            ry = ly;
            return outputs[handle]->output.get();
        }

        wayfire_output *get_output_at(int x, int y)
        {
            int dummy_x, dummy_y;
            return get_output_coords_at(x, y, dummy_x, dummy_y);
        }

        bool apply_configuration(const output_configuration_t& configuration, bool test_only)
        {
            bool ok = test_configuration(configuration);
            if (ok && !test_only)
                apply_configuration(configuration);

            return ok;
        }
    };

    /* Just pass to the PIMPL */
    output_layout_t::output_layout_t(wlr_backend *b) : pimpl(new impl(b)) {}
    output_layout_t::~output_layout_t() = default;
    wlr_output_layout *output_layout_t::get_handle()
    { return pimpl->get_handle(); }
    wayfire_output *output_layout_t::get_output_at(int x, int y)
    { return pimpl->get_output_at(x, y); }
    wayfire_output *output_layout_t::get_output_coords_at(int x, int y, int& lx, int& ly)
    { return pimpl->get_output_coords_at(x, y, lx, ly); }
    size_t output_layout_t::get_num_outputs()
    { return pimpl->get_num_outputs(); }
    std::vector<wayfire_output*> output_layout_t::get_outputs()
    { return pimpl->get_outputs(); }
    wayfire_output *output_layout_t::get_next_output(wayfire_output *output)
    { return pimpl->get_next_output(output); }
    wayfire_output *output_layout_t::find_output(wlr_output *output)
    { return pimpl->find_output(output); }
    wayfire_output *output_layout_t::find_output(std::string name)
    { return pimpl->find_output(name); }
    output_configuration_t output_layout_t::get_current_configuration()
    { return pimpl->get_current_configuration(); }
    bool output_layout_t::apply_configuration(const output_configuration_t& configuration, bool test_only)
    { return pimpl->apply_configuration(configuration, test_only); }
}
