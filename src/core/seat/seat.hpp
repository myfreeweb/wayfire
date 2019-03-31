#ifndef SEAT_HPP
#define SEAT_HPP

extern "C"
{
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_switch.h>
}

#include "output.hpp"
#include "input-device.hpp"

struct wf_drag_icon : public wayfire_surface_t
{
    wlr_drag_icon *icon;
    wf::wl_listener_wrapper on_map, on_unmap, on_destroy;

    wf_drag_icon(wlr_drag_icon *icon);

    bool is_subsurface() { return true ;}
    wf_point get_output_position();
    void damage(const wlr_box& rect);
};

class wf_input_device_internal : public wf::input_device_t
{
    public:
    wf_input_device_internal(wlr_input_device* dev);

    wf::wl_listener_wrapper on_switch, on_destroy;

    void handle_switched(wlr_event_switch_toggle *ev);
    void update_options();

    static struct config_t
    {
        wf_option mouse_cursor_speed;
        wf_option mouse_scroll_speed;
        wf_option touchpad_cursor_speed;
        wf_option touchpad_scroll_speed;
        wf_option touchpad_tap_enabled;
        wf_option touchpad_click_method;
        wf_option touchpad_scroll_method;
        wf_option touchpad_dwt_enabled;
        wf_option touchpad_dwmouse_enabled;
        wf_option touchpad_natural_scroll_enabled;

        void load(wayfire_config *config);
    } config;
};

#endif /* end of include guard: SEAT_HPP */
