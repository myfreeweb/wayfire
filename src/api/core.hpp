#ifndef CORE_HPP
#define CORE_HPP

#include "util.hpp"
#include "object.hpp"
#include "input-device.hpp"
#include "output-layout.hpp"

#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <set>
#include <nonstd/observer_ptr.h>

extern "C"
{
#include <wlr/backend.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>

#define static
#include <wlr/types/wlr_compositor.h>
#undef static

struct wlr_data_device_manager;
struct wlr_data_control_manager_v1;
struct wlr_linux_dmabuf_v1;
struct wlr_gamma_control_manager;
struct wlr_gamma_control_manager_v1;
struct wlr_screenshooter;
struct wlr_xdg_output_manager_v1;
struct wlr_export_dmabuf_manager_v1;
struct wlr_server_decoration_manager;
struct wlr_input_inhibit_manager;
struct wayfire_shell;
struct wf_gtk_shell;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_idle;
struct wlr_idle_inhibit_manager_v1;
struct wlr_screencopy_manager_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_pointer_gestures_v1;

#include <wayland-server.h>
}

class decorator_base_t;
class input_manager;
class wayfire_config;
class wayfire_output;
class wayfire_view_t;
class wayfire_surface_t;

using wayfire_view = nonstd::observer_ptr<wayfire_view_t>;

struct wf_server_decoration;

class wayfire_core : public wf_object_base
{
        friend struct plugin_manager;
        friend class wayfire_output;

        wf::wl_listener_wrapper output_layout_changed;
        wf::wl_listener_wrapper decoration_created;
        wf::wl_listener_wrapper vkbd_created;
        wf::wl_listener_wrapper input_inhibit_activated;
        wf::wl_listener_wrapper input_inhibit_deactivated;

        wayfire_output *active_output = nullptr;
        std::vector<std::unique_ptr<wayfire_view_t>> views;

        void configure(wayfire_config *config);

        int times_wake = 0;
        /* pairs (layer, request_id) */
        std::set<std::pair<uint32_t, int>> layer_focus_requests;

    public:
        std::map<wlr_surface*, uint32_t> uses_csd;
        wayfire_config *config;

        wl_display *display;
        wl_event_loop *ev_loop;
        wlr_backend *backend;
        wlr_egl *egl;
        wlr_renderer *renderer;
        wlr_compositor *compositor;
        std::unique_ptr<wf::output_layout_t> output_layout;

        struct
        {
            wlr_data_device_manager *data_device;
            wlr_data_control_manager_v1 *data_control;
            wlr_gamma_control_manager *gamma;
            wlr_gamma_control_manager_v1 *gamma_v1;
            wlr_screenshooter *screenshooter;
            wlr_screencopy_manager_v1 *screencopy;
            wlr_linux_dmabuf_v1 *linux_dmabuf;
            wlr_export_dmabuf_manager_v1 *export_dmabuf;
            wlr_server_decoration_manager *decorator_manager;
            wlr_xdg_output_manager_v1 *output_manager;
            wlr_virtual_keyboard_manager_v1 *vkbd_manager;
            wlr_input_inhibit_manager *input_inhibit;
            wlr_idle *idle;
            wlr_idle_inhibit_manager_v1 *idle_inhibit;
            wlr_foreign_toplevel_manager_v1 *toplevel_manager;
            wlr_pointer_gestures_v1 *pointer_gestures;
            wayfire_shell *wf_shell;
            wf_gtk_shell *gtk_shell;
        } protocols;

        std::string wayland_display, xwayland_display;

        input_manager *input;

        std::string to_string() const { return "wayfire-core"; }

        void init(wayfire_config *config);
        void wake();
        void sleep();
        void refocus_active_output_active_view();

        wlr_seat *get_current_seat();

        uint32_t get_keyboard_modifiers();

        /* Set the cursor to the given name from the cursor theme, if available */
        void set_cursor(std::string name);
        /* Hides the cursor, until something sets it up again, for ex. by set_cursor() */
        void hide_cursor();
        /* Sends an absolute motion event. x and y should be passed in global coordinates */
        void warp_cursor(int x, int y);

        /* no such coordinate will ever realistically be used for input */
        static const int invalid_coordinate = -123456789;

        /* in output-layout-local coordinates */
        std::tuple<int, int> get_cursor_position();

        /* in output-layout-local coordinates */
        std::tuple<int, int> get_touch_position(int id);

        wayfire_surface_t *get_cursor_focus();
        wayfire_surface_t *get_touch_focus();

        std::vector<nonstd::observer_ptr<wf::input_device_t>> get_input_devices();

        void add_view(std::unique_ptr<wayfire_view_t> view);

        wayfire_view find_view(wayfire_surface_t *);
        wayfire_view find_view(uint32_t id);

        /* completely destroy a view */
        void erase_view(wayfire_view view);

        /* brings the view to the top and also focuses its output */
        void focus_view(wayfire_view win, wlr_seat *seat);
        /**
         * Change the view's output to new_output. However, the view geometry
         * isn't changed - the caller needs to make sure that the view doesn't
         * become unreachable, for ex. by going out of the output bounds
         */
        void move_view_to_output(wayfire_view v, wayfire_output *new_output);

        void focus_output(wayfire_output *o);
        wayfire_output *get_active_output();
        /* Add a request to focus the given layer, or update an existing request.
         * Returns the UID of the request which was added/modified.
         *
         * Calling this with request >= 0 will have no effect if the given
         * request doesn't exist, in which case -1 is returned */
        int focus_layer(uint32_t layer, int request);
        /* Removes a request from the list. No-op for requests that do not exist
         * currently or for request < 0 */
        void unfocus_layer(int request);
        uint32_t get_focused_layer();

        void run(const char *command);

        int vwidth, vheight;

        std::string shadersrc;
        bool run_panel;
};

extern wayfire_core *core;
#endif // CORE_HPP
