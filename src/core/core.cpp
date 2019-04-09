extern "C"
{
#include <wlr/config.h>
#include <wlr/types/wlr_screenshooter.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_gamma_control.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
}

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "debug.hpp"
#include "output.hpp"
#include "core.hpp"
#include "signal-definitions.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include "seat/input-manager.hpp"
#include "seat/input-inhibit.hpp"
#include "seat/touch.hpp"
#include "../output/wayfire-shell.hpp"
#include "../output/gtk-shell.hpp"
#include "view/priv-view.hpp"
#include "config.h"
#include "img.hpp"

/* End input_manager */

void wayfire_core::configure(wayfire_config *config)
{
    this->config = config;
    auto section = config->get_section("core");

    vwidth  = *section->get_option("vwidth", "3");
    vheight = *section->get_option("vheight", "3");
}

/* decorations impl */
struct wf_server_decoration
{
    wlr_server_decoration *decor;
    wf::wl_listener_wrapper on_mode_set, on_destroy;

    std::function<void(void*)> mode_set = [&] (void*)
    {
        log_debug("set decoration mode %d", decor->mode);
        bool use_csd = decor->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
        core->uses_csd[decor->surface] = use_csd;

        auto wf_surface = wf_surface_from_void(decor->surface->data);
        if (wf_surface)
            wf_surface->has_client_decoration = use_csd;
    };

    wf_server_decoration(wlr_server_decoration *_decor)
        : decor(_decor)
    {
        on_mode_set.set_callback(mode_set);
        on_destroy.set_callback([&] (void *) {
            core->uses_csd.erase(decor->surface);
            delete this;
        });

        on_mode_set.connect(&decor->events.mode);
        on_destroy.connect(&decor->events.destroy);
        /* Read initial decoration settings */
        mode_set(NULL);
    }
};

void wayfire_core::init(wayfire_config *conf)
{
    configure(conf);
    wf_input_device_internal::config.load(conf);

    protocols.data_device = wlr_data_device_manager_create(display);
    protocols.data_control = wlr_data_control_manager_v1_create(display);
    wlr_renderer_init_wl_display(renderer, display);

    input = new input_manager();
    output_layout = std::make_unique<wf::output_layout_t> (backend);
    core->compositor = wlr_compositor_create(display, wlr_backend_get_renderer(backend));
    init_desktop_apis();

    protocols.screenshooter = wlr_screenshooter_create(display);
    protocols.screencopy = wlr_screencopy_manager_v1_create(display);
    protocols.gamma = wlr_gamma_control_manager_create(display);
    protocols.gamma_v1 = wlr_gamma_control_manager_v1_create(display);
    protocols.linux_dmabuf = wlr_linux_dmabuf_v1_create(display, renderer);
    protocols.export_dmabuf = wlr_export_dmabuf_manager_v1_create(display);
    protocols.output_manager = wlr_xdg_output_manager_v1_create(display,
        output_layout->get_handle());

    /* input-inhibit setup */
    protocols.input_inhibit = create_input_inhibit();
    input_inhibit_activated.set_callback([&] (void*) {
        input->set_exclusive_focus(protocols.input_inhibit->active_client); });
    input_inhibit_activated.connect(&protocols.input_inhibit->events.activate);

    input_inhibit_deactivated.set_callback([&] (void*) {
        input->set_exclusive_focus(nullptr); });
    input_inhibit_deactivated.connect(&protocols.input_inhibit->events.deactivate);

    /* decoration_manager setup */
    protocols.decorator_manager = wlr_server_decoration_manager_create(display);
    wlr_server_decoration_manager_set_default_mode(protocols.decorator_manager,
                                                   WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);

    decoration_created.set_callback([&] (void* data) {
        /* will be freed by the destroy request */
        new wf_server_decoration((wlr_server_decoration*)(data));});
    decoration_created.connect(&protocols.decorator_manager->events.new_decoration);

    protocols.vkbd_manager = wlr_virtual_keyboard_manager_v1_create(display);
    vkbd_created.set_callback([&] (void *data) {
        auto kbd = (wlr_virtual_keyboard_v1*) data;
        input->handle_new_input(&kbd->input_device);
    });
    vkbd_created.connect(&protocols.vkbd_manager->events.new_virtual_keyboard);

    protocols.idle = wlr_idle_create(display);
    protocols.idle_inhibit = wlr_idle_inhibit_v1_create(display);

    protocols.wf_shell = wayfire_shell_create(display);
    protocols.gtk_shell = wf_gtk_shell_create(display);
    protocols.toplevel_manager = wlr_foreign_toplevel_manager_v1_create(display);
    protocols.pointer_gestures = wlr_pointer_gestures_v1_create(display);

    image_io::init();
    OpenGL::init();
}

void wayfire_core::wake()
{
    static wf::wl_idle_call idle_refocus;
    idle_refocus.run_once([&] () {refocus_active_output_active_view(); });

    if (times_wake > 0)
    {
        for (auto& wo : output_layout->get_outputs())
            wo->emit_signal("wake", nullptr);
    }

    ++times_wake;
}

void wayfire_core::sleep()
{
    for (auto& wo : output_layout->get_outputs())
        wo->emit_signal("sleep", nullptr);
}

wlr_seat* wayfire_core::get_current_seat()
{ return input->seat; }

uint32_t wayfire_core::get_keyboard_modifiers()
{
    return input->get_modifiers();
}

void wayfire_core::set_cursor(std::string name)
{
    input->cursor->set_cursor(name);
}

void wayfire_core::hide_cursor()
{
    input->cursor->hide_cursor();
}

void wayfire_core::warp_cursor(int x, int y)
{
    input->cursor->warp_cursor(x, y);
}

const int wayfire_core::invalid_coordinate;
std::tuple<int, int> wayfire_core::get_cursor_position()
{
    if (input->cursor)
        return std::tuple<int, int> (input->cursor->cursor->x, input->cursor->cursor->y);
    else
        return std::tuple<int, int> (invalid_coordinate, invalid_coordinate);
}

std::tuple<int, int> wayfire_core::get_touch_position(int id)
{
    if (!input->our_touch)
        return std::make_tuple(invalid_coordinate, invalid_coordinate);

    auto it = input->our_touch->gesture_recognizer.current.find(id);
    if (it != input->our_touch->gesture_recognizer.current.end())
        return std::make_tuple(it->second.sx, it->second.sy);

    return std::make_tuple(invalid_coordinate, invalid_coordinate);
}

wayfire_surface_t *wayfire_core::get_cursor_focus()
{
    return input->cursor_focus;
}

wayfire_surface_t *wayfire_core::get_touch_focus()
{
    return input->touch_focus;
}

std::vector<nonstd::observer_ptr<wf::input_device_t>>
wayfire_core::get_input_devices()
{
    std::vector<nonstd::observer_ptr<wf::input_device_t>> list;
    for (auto& dev : input->input_devices)
        list.push_back(nonstd::make_observer(dev.get()));

    return list;
}

void wayfire_core::refocus_active_output_active_view()
{
    if (!active_output)
        return;

    auto view = active_output->get_active_view();
    if (view) {
        active_output->focus_view(nullptr);
        active_output->focus_view(view);
    }
}

void wayfire_core::focus_output(wayfire_output *wo)
{
    assert(wo);
    if (active_output == wo)
        return;

    wo->ensure_pointer();

    wayfire_grab_interface old_grab = nullptr;

    if (active_output)
    {
        old_grab = active_output->get_input_grab_interface();
        active_output->focus_view(nullptr);
    }

    active_output = wo;
    log_debug("focusing %p", wo);
    if (wo)
        log_debug("focus output: %s", wo->handle->name);

    /* invariant: input is grabbed only if the current output
     * has an input grab */
    if (input->input_grabbed())
    {
        assert(old_grab);
        input->ungrab_input();
    }

    wayfire_grab_interface iface = wo->get_input_grab_interface();
    /* this cannot be recursion as active_output will be equal to wo,
     * and wo->active_view->output == wo */
    if (!iface)
        refocus_active_output_active_view();
    else
        input->grab_input(iface);

    if (active_output)
    {
        wlr_output_schedule_frame(active_output->handle);
        active_output->emit_signal("output-gain-focus", nullptr);
    }
}

wayfire_output* wayfire_core::get_active_output()
{
    return active_output;
}

int wayfire_core::focus_layer(uint32_t layer, int32_t request_uid_hint)
{
    static int32_t last_request_uid = -1;
    if (request_uid_hint >= 0)
    {
        /* Remove the old request, and insert the new one */
        uint32_t old_layer = -1;
        for (auto& req : layer_focus_requests)
        {
            if (req.second == request_uid_hint)
                old_layer = req.first;
        }

        /* Request UID isn't valid */
        if (old_layer == (uint32_t)-1)
            return -1;

        layer_focus_requests.erase({old_layer, request_uid_hint});
    }

    auto request_uid = request_uid_hint < 0 ?
        ++last_request_uid : request_uid_hint;
    layer_focus_requests.insert({layer, request_uid});
    log_debug("focusing layer %d", get_focused_layer());

    active_output->refocus();
    return request_uid;
}

uint32_t wayfire_core::get_focused_layer()
{
    if (layer_focus_requests.empty())
        return 0;

    return (--layer_focus_requests.end())->first;
}

void wayfire_core::unfocus_layer(int request)
{
    for (auto& freq : layer_focus_requests)
    {
        if (freq.second == request)
        {
            layer_focus_requests.erase(freq);
            log_debug("focusing layer %d", get_focused_layer());

            active_output->refocus(nullptr);
            return;
        }
    }
}

void wayfire_core::add_view(std::unique_ptr<wayfire_view_t> view)
{
    views.push_back(std::move(view));
    assert(active_output);
}

wayfire_view wayfire_core::find_view(wayfire_surface_t *handle)
{
    auto view = dynamic_cast<wayfire_view_t*> (handle);
    if (!view)
        return nullptr;

    return nonstd::make_observer(view);
}

wayfire_view wayfire_core::find_view(uint32_t id)
{
    for (auto& v : views)
        if (v->get_id() == id)
            return nonstd::make_observer(v.get());

    return nullptr;
}

void wayfire_core::focus_view(wayfire_view v, wlr_seat *seat)
{
    if (!v)
        return;

    if (v->get_output() != active_output)
        focus_output(v->get_output());

    active_output->focus_view(v, seat);
}

void wayfire_core::erase_view(wayfire_view v)
{
    if (!v) return;

    if (v->get_output())
        v->get_output()->detach_view(v);

    auto it = std::find_if(views.begin(), views.end(),
                           [&v] (const std::unique_ptr<wayfire_view_t>& k)
                           { return k.get() == v.get(); });

    views.erase(it);
}

void wayfire_core::run(const char *command)
{
    pid_t pid = fork();

    /* The following is a "hack" for disowning the child processes,
     * otherwise they will simply stay as zombie processes */
    if (!pid) {
        if (!fork()) {
            setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);
            setenv("WAYLAND_DISPLAY", wayland_display.c_str(), 1);
#if WLR_HAS_XWAYLAND
            auto xdisp = ":" + xwayland_get_display();
            setenv("DISPLAY", xdisp.c_str(), 1);
#endif
            int dev_null = open("/dev/null", O_WRONLY);
            dup2(dev_null, 1);
            dup2(dev_null, 2);

            exit(execl("/bin/sh", "/bin/bash", "-c", command, NULL));
        } else {
            exit(0);
        }
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}

void wayfire_core::move_view_to_output(wayfire_view v, wayfire_output *new_output)
{
    assert(new_output);
    if (v->get_output())
        v->get_output()->detach_view(v);

    new_output->attach_view(v);
    new_output->focus_view(v);
}

wayfire_core *core;
