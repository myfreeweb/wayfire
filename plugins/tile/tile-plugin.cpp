#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#include "tree-controller.hpp"
#include "../single_plugins/view-change-viewport-signal.hpp"
#include "../matcher/matcher.hpp"

namespace wf
{
class tile_workspace_implementation_t : public wf::workspace_implementation_t
{
  public:
    bool view_movable(wayfire_view view) override
    {
        return wf::tile::view_node_t::get_node(view) == nullptr;
    }

    bool view_resizable(wayfire_view view) override
    {
        return wf::tile::view_node_t::get_node(view) == nullptr;
    }
};

class tile_plugin_t : public wf::plugin_interface_t
{
  private:
    std::unique_ptr<wf::matcher::view_matcher> tile_by_default_matcher;
    wf::option_wrapper_t<std::string> tile_by_default{"simple-tile/tile_by_default"};
    wf::option_wrapper_t<bool> keep_fullscreen_on_adjacent{"simple-tile/keep_fullscreen_on_adjacent"};
    wf::option_wrapper_t<wf::buttonbinding_t> button_move{"simple-tile/button_move"}, button_resize{"simple-tile/button_resize"};
    wf::option_wrapper_t<wf::keybinding_t> key_toggle_tile{"simple-tile/key_toggle"}, key_toggle_fullscreen{"simple-tile/key_toggle_fullscreen"};

    wf::option_wrapper_t<wf::keybinding_t> key_focus_left{"simple-tile/key_focus_left"}, key_focus_right{"simple-tile/key_focus_right"};
    wf::option_wrapper_t<wf::keybinding_t> key_focus_above{"simple-tile/key_focus_above"}, key_focus_below{"simple-tile/key_focus_below"};

  private:
    std::vector<std::vector<std::unique_ptr<wf::tile::tree_node_t>>> roots;

    const wf::tile::split_direction_t default_split = wf::tile::SPLIT_VERTICAL;

    void initialize_roots()
    {
        auto wsize = output->workspace->get_workspace_grid_size();
        roots.resize(wsize.width);
        for (int i = 0; i < wsize.width; i++)
        {
            roots[i].resize(wsize.height);
            for (int j = 0; j < wsize.height; j++)
            {
                roots[i][j] =
                    std::make_unique<wf::tile::split_node_t>(default_split);
            }
        }

        update_root_size(output->workspace->get_workarea());
    }

    void update_root_size(wf::geometry_t workarea)
    {
        auto output_geometry = output->get_relative_geometry();
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j = 0; j < wsize.height; j++)
            {
                /* Set size */
                auto vp_geometry = workarea;
                vp_geometry.x += i * output_geometry.width;
                vp_geometry.y += j * output_geometry.height;
                roots[i][j]->set_geometry(vp_geometry);
            }
        }
    }

    void flatten_roots()
    {
        for (auto& col : roots)
        {
            for (auto& root : col)
                tile::flatten_tree(root);
        }
    }

    bool can_tile_view(wayfire_view view)
    {
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
            return false;

        if (view->parent)
            return false;

        return true;
    }

    static std::unique_ptr<wf::tile::tile_controller_t> get_default_controller()
    {
        return std::make_unique<wf::tile::tile_controller_t> ();
    }

    std::unique_ptr<wf::tile::tile_controller_t> controller =
        get_default_controller();

    /**
     * Translate coordinates from output-local coordinates to the coordinate
     * system of the tiling trees, depending on the current workspace
     */
    wf::point_t get_global_coordinates(wf::point_t local_coordinates)
    {
        auto vp = output->workspace->get_current_workspace();
        auto size = output->get_screen_size();
        local_coordinates.x += size.width * vp.x;
        local_coordinates.y += size.height * vp.y;

        return local_coordinates;
    }

    /** Check whether we currently have a fullscreen tiled view */
    bool has_fullscreen_view()
    {
        auto vp = output->workspace->get_current_workspace();

        int count_fullscreen = 0;
        for_each_view(roots[vp.x][vp.y], [&] (wayfire_view view) {
            count_fullscreen += view->fullscreen;
        });

        return count_fullscreen > 0;
    }

    /** Check whether the current pointer focus is tiled view */
    bool has_tiled_focus()
    {
        auto focus = wf::get_core().get_cursor_focus_view();
        return focus && tile::view_node_t::get_node(focus);
    }

    template<class Controller>
    bool start_controller(wf::point_t grab)
    {
        /* No action possible in this case */
        if (has_fullscreen_view() || !has_tiled_focus())
            return false;

        if (!output->activate_plugin(grab_interface))
            return false;

        if (grab_interface->grab())
        {
            auto vp = output->workspace->get_current_workspace();
            controller = std::make_unique<Controller> (
                roots[vp.x][vp.y], get_global_coordinates(grab));
        } else
        {
            output->deactivate_plugin(grab_interface);
        }

        return true;
    }

    void stop_controller(bool force_stop)
    {
        if (!output->is_plugin_active(grab_interface->name))
            return;

        if (!force_stop)
            controller->input_released();

        output->deactivate_plugin(grab_interface);
        controller = get_default_controller();
    }

    void attach_view(wayfire_view view, wf::point_t vp = {-1, -1})
    {
        if (!can_tile_view(view))
            return;

        stop_controller(true);

        if (vp == wf::point_t{-1, -1})
            vp = output->workspace->get_current_workspace();

        auto view_node = std::make_unique<wf::tile::view_node_t> (view);
        roots[vp.x][vp.y]->as_split_node()->add_child(std::move(view_node));

        tile::restack_output_workspace(output,
            output->workspace->get_current_workspace());
    }

    bool tile_window_by_default(wayfire_view view)
    {
        return wf::matcher::evaluate(tile_by_default_matcher, view) &&
            can_tile_view(view);
    }

    signal_callback_t on_view_attached = [=] (signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        if (tile_window_by_default(view))
            attach_view(view);
    };

    signal_callback_t on_view_unmapped = [=] (signal_data_t *data)
    {
        stop_controller(true);
    };

    /** Remove the given view from its tiling container */
    void detach_view(nonstd::observer_ptr<tile::view_node_t> view)
    {
        stop_controller(true);
        auto wview = view->view;

        view->parent->remove_child(view);
        /* View node is invalid now */
        flatten_roots();

        if (wview->fullscreen && wview->is_mapped())
            wview->fullscreen_request(nullptr, false);
    }

    signal_callback_t on_view_detached = [=] (signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto view_node = wf::tile::view_node_t::get_node(view);

        if (view_node)
            detach_view(view_node);
    };

    signal_callback_t on_workarea_changed = [=] (signal_data_t *data)
    {
        update_root_size(output->workspace->get_workarea());
    };

    signal_callback_t on_tile_request = [=] (signal_data_t *data)
    {
        auto ev = static_cast<view_tiled_signal*> (data);
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
            return;

        // we ignore those requests because we manage the tiled state manually
        ev->carried_out = true;
    };

    void set_view_fullscreen(wayfire_view view, bool fullscreen)
    {
        /* Set fullscreen, and trigger resizing of the views */
        view->set_fullscreen(fullscreen);
        update_root_size(output->workspace->get_workarea());
    }

    signal_callback_t on_fullscreen_request = [=] (signal_data_t *data)
    {
        auto ev = static_cast<view_fullscreen_signal*> (data);
        if (ev->carried_out || !tile::view_node_t::get_node(ev->view))
            return;

        ev->carried_out = true;
        set_view_fullscreen(ev->view, ev->state);
    };

    signal_callback_t on_focus_changed = [=] (signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        if (tile::view_node_t::get_node(view) && !view->fullscreen)
        {
            auto vp = output->workspace->get_current_workspace();
            for_each_view(roots[vp.x][vp.y], [&] (wayfire_view view) {
                if (view->fullscreen)
                    set_view_fullscreen(view, false);
            });
        }

        tile::restack_output_workspace(output,
            output->workspace->get_current_workspace());
    };

    void change_view_workspace(wayfire_view view, wf::point_t vp = {-1, -1})
    {
        auto existing_node = wf::tile::view_node_t::get_node(view);
        if (existing_node)
        {
            detach_view(existing_node);
            attach_view(view, vp);
        }
    }

    signal_callback_t on_view_change_viewport = [=] (signal_data_t *data)
    {
        auto ev = (view_change_viewport_signal*) (data);
        change_view_workspace(ev->view, ev->to);
    };

    signal_callback_t on_view_minimized = [=] (signal_data_t *data)
    {
        auto ev = (view_minimize_request_signal*) data;
        auto existing_node = wf::tile::view_node_t::get_node(ev->view);

        if (ev->state && existing_node)
            detach_view(existing_node);

        if (!ev->state && tile_window_by_default(ev->view))
            attach_view(ev->view);
    };

    /**
     * Execute the given function on the focused view iff we can activate the
     * tiling plugin, there is a focused view and the focused view is a tiled
     * view
     *
     * @param need_tiled Whether the view needs to be tiled
     */
    bool conditioned_view_execute(bool need_tiled,
        std::function<void(wayfire_view)> func)
    {
        auto view = output->get_active_view();
        if (!view)
            return false;

        if (need_tiled && !tile::view_node_t::get_node(view))
            return false;

        if (output->activate_plugin(grab_interface))
        {
            func(view);
            output->deactivate_plugin(grab_interface);

            return true;
        }

        return false;
    }

    wf::key_callback on_toggle_fullscreen = [=] (uint32_t key)
    {
        return conditioned_view_execute(true, [=] (wayfire_view view)
        {
            stop_controller(true);
            set_view_fullscreen(view, !view->fullscreen);
        });
    };

    wf::key_callback on_toggle_tiled_state = [=] (uint32_t key)
    {
        return conditioned_view_execute(false, [=] (wayfire_view view)
        {
            auto existing_node = tile::view_node_t::get_node(view);
            if (existing_node) {
                detach_view(existing_node);
                view->tile_request(0);
            } else {
                attach_view(view);
            }
        });
    };

    bool focus_adjacent(tile::split_insertion_t direction)
    {
        return conditioned_view_execute(true, [=] (wayfire_view view)
        {
            auto adjacent = tile::find_first_view_in_direction(
                tile::view_node_t::get_node(view), direction);

            bool was_fullscreen = view->fullscreen;
            if (adjacent)
            {
                /* This will lower the fullscreen status of the view */
                output->focus_view(adjacent->view, true);

                if (was_fullscreen && keep_fullscreen_on_adjacent)
                    adjacent->view->fullscreen_request(output, true);
            }
        });
    }

    wf::key_callback on_focus_adjacent = [=] (uint32_t key)
    {
        if (key == ((wf::keybinding_t)key_focus_left).get_key())
            return focus_adjacent(tile::INSERT_LEFT);
        if (key == ((wf::keybinding_t)key_focus_right).get_key())
            return focus_adjacent(tile::INSERT_RIGHT);
        if (key == ((wf::keybinding_t)key_focus_above).get_key())
            return focus_adjacent(tile::INSERT_ABOVE);
        if (key == ((wf::keybinding_t)key_focus_below).get_key())
            return focus_adjacent(tile::INSERT_BELOW);

        return false;
    };

    wf::button_callback on_move_view = [=] (uint32_t button, int32_t x, int32_t y)
    {
        return start_controller<tile::move_view_controller_t> ({x, y});
    };

    wf::button_callback on_resize_view = [=] (uint32_t button, int32_t x, int32_t y)
    {
        return start_controller<tile::resize_view_controller_t> ({x, y});
    };

    void setup_callbacks()
    {
        output->add_button(button_move, &on_move_view);
        output->add_button(button_resize, &on_resize_view);
        output->add_key(key_toggle_tile, &on_toggle_tiled_state);
        output->add_key(key_toggle_fullscreen, &on_toggle_fullscreen);

        output->add_key(key_focus_left,  &on_focus_adjacent);
        output->add_key(key_focus_right, &on_focus_adjacent);
        output->add_key(key_focus_above, &on_focus_adjacent);
        output->add_key(key_focus_below, &on_focus_adjacent);

        grab_interface->callbacks.pointer.button =
            [=] (uint32_t b, uint32_t state)
            {
                if (state == WLR_BUTTON_RELEASED)
                    stop_controller(false);
            };

        grab_interface->callbacks.pointer.motion = [=] (int32_t x, int32_t y)
        {
            controller->input_motion(get_global_coordinates({x, y}));
        };
    }

  public:
    void init() override
    {
        this->grab_interface->name = "simple-tile";
        /* TODO: change how grab interfaces work - plugins should do ifaces on
         * their own, and should be able to have more than one */
        this->grab_interface->capabilities = CAPABILITY_MANAGE_COMPOSITOR;

        initialize_roots();
        // TODO: check whether this was successful
        output->workspace->set_workspace_implementation(
            std::make_unique<tile_workspace_implementation_t> (), true);

        output->connect_signal("unmap-view", &on_view_unmapped);
        output->connect_signal("layer-attach-view", &on_view_attached);
        output->connect_signal("layer-detach-view", &on_view_detached);
        output->connect_signal("reserved-workarea", &on_workarea_changed);
        output->connect_signal("view-maximized-request", &on_tile_request);
        output->connect_signal("view-fullscreen-request",
            &on_fullscreen_request);
        output->connect_signal("focus-view", &on_focus_changed);
        output->connect_signal("view-change-viewport", &on_view_change_viewport);
        output->connect_signal("view-minimize-request", &on_view_minimized);

        tile_by_default_matcher = wf::matcher::get_matcher(tile_by_default);
        setup_callbacks();
    }

    void fini() override
    {
        output->workspace->set_workspace_implementation(nullptr, true);

        output->rem_binding(&on_move_view);
        output->rem_binding(&on_resize_view);
        output->rem_binding(&on_toggle_fullscreen);
        output->rem_binding(&on_toggle_tiled_state);
        output->rem_binding(&on_focus_adjacent);

        output->disconnect_signal("unmap-view", &on_view_unmapped);
        output->disconnect_signal("layer-attach-view", &on_view_attached);
        output->disconnect_signal("layer-detach-view", &on_view_detached);
        output->disconnect_signal("reserved-workarea", &on_workarea_changed);
        output->disconnect_signal("view-maximized-request", &on_tile_request);
        output->disconnect_signal("view-fullscreen-request",
            &on_fullscreen_request);
        output->disconnect_signal("focus-view", &on_focus_changed);
        output->disconnect_signal("view-change-viewport",
            &on_view_change_viewport);
        output->disconnect_signal("view-minimize-request", &on_view_minimized);
    }
};
};

DECLARE_WAYFIRE_PLUGIN(wf::tile_plugin_t);
