#ifndef SEAT_HPP
#define SEAT_HPP

extern "C"
{
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/backend/libinput.h>
}

#include "output.hpp"
#include "input-device.hpp"

struct wf_drag_icon : public wayfire_surface_t
{
    wlr_drag_icon *icon;
    wl_listener map_ev, unmap_ev, destroy;

    wf_drag_icon(wlr_drag_icon *icon);

    bool is_subsurface() { return true ;}
    wf_point get_output_position();
    void damage(const wlr_box& rect);
};

class wf_input_device_internal : public wf::input_device_t
{
    public:
    wf_input_device_internal(wlr_input_device* dev);
    ~wf_input_device_internal();

    struct wlr_wrapper {
        wl_listener destroy;
        wf_input_device_internal* self;
    } _wrapper;

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
