#include <output.hpp>
#include <view.hpp>
#include <core.hpp>
#include <workspace-manager.hpp>
#include <linux/input.h>
#include <signal-definitions.hpp>

extern "C"
{
#include <wlr/types/wlr_xcursor_manager.h>
}

#include "../wobbly/wobbly-signal.hpp"

class wayfire_resize : public wayfire_plugin_t {
    signal_callback_t resize_request, view_destroyed;

    button_callback activate_binding;
    touch_callback touch_activate_binding;
    wayfire_view view;

    bool was_client_request;
    int initial_x, initial_y;
    int initial_width, initial_height;

    uint32_t edges;
    bool set_wobbly = false;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "resize";
        grab_interface->abilities_mask = WF_ABILITY_CHANGE_VIEW_GEOMETRY | WF_ABILITY_GRAB_INPUT;

        auto button = (*config)["resize"]->get_option("activate", "<super> BTN_LEFT");
        activate_binding = [=] (uint32_t, int x, int y)
        {
            auto focus = core->get_cursor_focus();
            auto view = focus ? core->find_view(focus->get_main_surface()) : nullptr;
            was_client_request = false;
            initiate(view, x, y);
        };

        touch_activate_binding = [=] (int32_t sx, int32_t sy)
        {
            auto focus = core->get_touch_focus();
            auto view = focus ? core->find_view(focus->get_main_surface()) : nullptr;
            was_client_request = false;
            initiate(view, sx, sy);
        };


        output->add_button(button, &activate_binding);
        output->add_touch(button->as_button().mod, &touch_activate_binding);

        grab_interface->callbacks.pointer.button = [=] (uint32_t b, uint32_t state)
        {
            if (state == WLR_BUTTON_RELEASED && was_client_request && b == BTN_LEFT)
                return input_pressed(state);

            if (b != button->as_cached_button().button)
                return;

            input_pressed(state);
        };

        grab_interface->callbacks.pointer.motion = [=] (int x, int y)
        {
            input_motion(x, y);
        };

        grab_interface->callbacks.touch.up = [=] (int32_t id)
        {
            if (id == 0)
                input_pressed(WLR_BUTTON_RELEASED);
        };

        grab_interface->callbacks.touch.motion = [=] (int32_t id, int32_t sx, int32_t sy)
        {
            if (id == 0)
                input_motion(sx, sy);
        };

        grab_interface->callbacks.cancel = [=] ()
        {
            input_pressed(WLR_BUTTON_RELEASED);
        };

        using namespace std::placeholders;
        resize_request = std::bind(std::mem_fn(&wayfire_resize::resize_requested),
                this, _1);
        output->connect_signal("resize-request", &resize_request);

        view_destroyed = [=] (signal_data* data)
        {
            if (get_signaled_view(data) == view)
            {
                view = nullptr;
                input_pressed(WLR_BUTTON_RELEASED);
            }
        };

        output->connect_signal("detach-view", &view_destroyed);
        output->connect_signal("unmap-view", &view_destroyed);
    }

    void resize_requested(signal_data *data)
    {
        auto request = static_cast<resize_request_signal*> (data);
        auto view = get_signaled_view(data);

        if (!view)
            return;

        int sx, sy;

        GetTuple(tx, ty, core->get_touch_position(0));
        if (tx != wayfire_core::invalid_coordinate &&
            ty != wayfire_core::invalid_coordinate)
        {
            sx = tx;
            sy = ty;
        } else
        {
            GetTuple(px, py, core->get_cursor_position());
            sx = px;
            sy = py;
        }

        was_client_request = true;
        initiate(view, sx, sy, request->edges);
    }

    void initiate(wayfire_view view, int sx, int sy, uint32_t forced_edges = 0)
    {
        set_wobbly = false;
        if (!view || view->role == WF_VIEW_ROLE_SHELL_VIEW || view->destroyed)
            return;

        if (!output->workspace->
                get_implementation(output->workspace->get_current_workspace())->
                    view_resizable(view))
            return;

        if (!output->activate_plugin(grab_interface))
            return;
        if (!grab_interface->grab()) {
            output->deactivate_plugin(grab_interface);
            return;
        }

        initial_x = sx;
        initial_y = sy;

        auto wm = view->get_wm_geometry();
        initial_width = wm.width;
        initial_height = wm.height;

        if (forced_edges == 0)
        {
            auto vg = view->get_wm_geometry();

            int view_x = initial_x - vg.x;
            int view_y = initial_y - vg.y;

            edges = 0;
            if (view_x < vg.width / 2) {
                edges |= WLR_EDGE_LEFT;
            } else {
                edges |= WLR_EDGE_RIGHT;
            }

            if (view_y < vg.height / 2) {
                edges |= WLR_EDGE_TOP;
            } else {
                edges |= WLR_EDGE_BOTTOM;
            }
        } else {
            edges = forced_edges;
        }

        if ((edges & WLR_EDGE_LEFT) ||
            (edges & WLR_EDGE_TOP))
            view->set_moving(true);

        view->set_resizing(true, edges);

        if (view->fullscreen)
            view->set_fullscreen(false);
        if (view->maximized)
            view->set_maximized(false);

        if (edges == 0) /* simply deactivate */
            input_pressed(WL_POINTER_BUTTON_STATE_RELEASED);
        this->view = view;

        auto og = view->get_output_geometry();
        int anchor_x = og.x;
        int anchor_y = og.y;

        if (edges & WLR_EDGE_LEFT)
            anchor_x += og.width;
        if (edges & WLR_EDGE_TOP)
            anchor_y += og.height;

        snap_wobbly(view, {}, false);
        start_wobbly(view, anchor_x, anchor_y);

        core->set_cursor(wlr_xcursor_get_resize_name((wlr_edges)edges));
    }

    void input_pressed(uint32_t state)
    {
        if (state != WL_POINTER_BUTTON_STATE_RELEASED)
            return;

        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);

        if (view)
        {
            if ((edges & WLR_EDGE_LEFT) ||
                (edges & WLR_EDGE_TOP))
                view->set_moving(false);
            view->set_resizing(false);
            end_wobbly(view);
        }
    }

    void input_motion(int sx, int sy)
    {
        int dx = sx - initial_x;
        int dy = sy - initial_y;
        int width = initial_width;
        int height = initial_height;

        if (edges & WLR_EDGE_LEFT)
            width -= dx;
        else if (edges & WLR_EDGE_RIGHT)
            width += dx;

        if (edges & WLR_EDGE_TOP)
            height -= dy;
        else if (edges & WLR_EDGE_BOTTOM)
            height += dy;

        height = std::max(height, 1);
        width  = std::max(width,  1);
        view->resize(width, height);

        auto og = view->get_output_geometry();
        int anchor_x = og.x;
        int anchor_y = og.y;

        if (edges & WLR_EDGE_LEFT)
            anchor_x += og.width;
        if (edges & WLR_EDGE_TOP)
            anchor_y += og.height;
    }

    void fini()
    {
        if (grab_interface->is_grabbed())
            input_pressed(WLR_BUTTON_RELEASED);

        output->rem_button(&activate_binding);
        output->rem_touch(&touch_activate_binding);

        output->disconnect_signal("resize-request", &resize_request);
        output->disconnect_signal("detach-view", &view_destroyed);
        output->disconnect_signal("unmap-view", &view_destroyed);
    }
};

extern "C" {
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_resize();
    }
}
