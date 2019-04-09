#include "debug.hpp"
#include "plugin-loader.hpp"
#include "output.hpp"
#include "view.hpp"
#include "core.hpp"
#include "signal-definitions.hpp"
#include "render-manager.hpp"
#include "workspace-manager.hpp"
#include "compositor-view.hpp"
#include "wayfire-shell.hpp"
#include "../core/seat/input-manager.hpp"

#include <linux/input.h>

#include <algorithm>
#include <assert.h>
#include <config.hpp>

wayfire_output::wayfire_output(wlr_output *handle, wayfire_config *c)
{
    this->handle = handle;

    render = new render_manager(this);
    plugin = new plugin_manager(this, c);

    view_disappeared_cb = [=] (signal_data *data) { refocus(get_signaled_view(data)); };
    connect_signal("view-disappeared", &view_disappeared_cb);
}

std::string wayfire_output::to_string() const
{
    return handle->name;
}

void wayfire_output::refocus(wayfire_view skip_view, uint32_t layers)
{
    wayfire_view next_focus = nullptr;
    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace(), layers, true);

    for (auto v : views)
    {
        if (v != skip_view && v->is_mapped() &&
            v->get_keyboard_focus_surface())
        {
            next_focus = v;
            break;
        }
    }

    set_active_view(next_focus);
}

void wayfire_output::refocus(wayfire_view skip_view)
{
    uint32_t focused_layer = core->get_focused_layer();
    uint32_t layers = focused_layer <= WF_LAYER_WORKSPACE ?
        WF_WM_LAYERS : focused_layer;

    auto views = workspace->get_views_on_workspace(
        workspace->get_current_workspace(), layers, true);

    if (views.empty())
    {
        if (core->get_active_output() == this)
            log_debug("warning: no focused views in the focused layer, probably a bug");

        /* Usually, we focus a layer so that a particular view has focus, i.e
         * we expect that there is a view in the focused layer. However we
         * should try to find reasonable focus in any focuseable layers if
         * that is not the case, for ex. if there is a focused layer by a
         * layer surface on another output */
        layers = wf_all_layers_not_below(focused_layer);
    }

    refocus(skip_view, layers);
}

workspace_manager::~workspace_manager()
{ }

wayfire_output::~wayfire_output()
{
    delete plugin;
    core->input->free_output_bindings(this);

    delete workspace;
    delete render;
}

wf_geometry wayfire_output::get_relative_geometry()
{
    wf_geometry g;
    g.x = g.y = 0;
    wlr_output_effective_resolution(handle, &g.width, &g.height);

    return g;
}

wf_geometry wayfire_output::get_layout_geometry()
{
    auto box = wlr_output_layout_get_box(core->output_layout->get_handle(), handle);
    if (box) {
        return *box;
    } else {
        log_error("Get layout geometry for an invalid output!");
        return {0, 0, 1, 1};
    }
}

std::tuple<int, int> wayfire_output::get_screen_size()
{
    int w, h;
    wlr_output_effective_resolution(handle, &w, &h);
    return std::make_tuple(w, h);
}

/* TODO: is this still relevant? */
void wayfire_output::ensure_pointer()
{
    /*
    auto ptr = weston_seat_get_pointer(core->get_current_seat());
    if (!ptr) return;

    int px = wl_fixed_to_int(ptr->x), py = wl_fixed_to_int(ptr->y);

    auto g = get_layout_geometry();
    if (!point_inside({px, py}, g)) {
        wl_fixed_t cx = wl_fixed_from_int(g.x + g.width / 2);
        wl_fixed_t cy = wl_fixed_from_int(g.y + g.height / 2);

        weston_pointer_motion_event ev;
        ev.mask |= WESTON_POINTER_MOTION_ABS;
        ev.x = wl_fixed_to_double(cx);
        ev.y = wl_fixed_to_double(cy);

        weston_pointer_move(ptr, &ev);
    } */
}

std::tuple<int, int> wayfire_output::get_cursor_position()
{
    GetTuple(x, y, core->get_cursor_position());
    auto og = get_layout_geometry();

    return std::make_tuple(x - og.x, y - og.y);
}

void wayfire_output::activate()
{
}

void wayfire_output::deactivate()
{
    // TODO: what do we do?
}

void wayfire_output::attach_view(wayfire_view v)
{
    v->set_output(this);
    workspace->add_view_to_layer(v, WF_LAYER_WORKSPACE);

    _view_signal data;
    data.view = v;
    emit_signal("attach-view", &data);
}

void wayfire_output::detach_view(wayfire_view v)
{
    _view_signal data;
    data.view = v;
    emit_signal("detach-view", &data);

    workspace->add_view_to_layer(v, 0);

    wayfire_view next = nullptr;
    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace(),
                                                   WF_MIDDLE_LAYERS, true);
    for (auto wview : views)
    {
        if (wview->is_mapped())
        {
            next = wview;
            break;
        }
    }

    if (next == nullptr)
    {
        active_view = nullptr;
    }
    else
    {
        focus_view(next);
    }
}

void wayfire_output::bring_to_front(wayfire_view v) {
    assert(v);

    workspace->add_view_to_layer(v, -1);
    v->damage();
}

/* sets the "active" view and gives it keyboard focus
 *
 * It maintains two different classes of "active views"
 * 1. active_view -> the view which has the current keyboard focus
 * 2. last_active_toplevel -> the toplevel view which last held the keyboard focus
 *
 * Because we don't want to deactivate views when for ex. a panel gets focus,
 * we don't deactivate the current view when this is the case. However, when the focus
 * goes back to the toplevel layer, we need to ensure the proper view is activated. */
void wayfire_output::set_active_view(wayfire_view v, wlr_seat *seat)
{
    if (v && !v->is_mapped())
        return set_active_view(nullptr, seat);

    if (seat == nullptr)
        seat = core->get_current_seat();

    bool refocus = (active_view == v);

    /* don't deactivate view if the next focus is not a toplevel */
    if (v == nullptr || v->role == WF_VIEW_ROLE_TOPLEVEL)
    {
        if (active_view && active_view->is_mapped() && !refocus)
            active_view->activate(false);

        /* make sure to deactivate the lastly activated toplevel */
        if (last_active_toplevel && v != last_active_toplevel)
            last_active_toplevel->activate(false);
    }

    active_view = v;

    /* If the output isn't focused, we shouldn't touch focus */
    if (core->get_active_output() == this)
    {
        if (active_view)
        {
            core->input->set_keyboard_focus(active_view, seat);

            if (!refocus)
                active_view->activate(true);
        } else
        {
            core->input->set_keyboard_focus(NULL, seat);

        }
    }

    if (!active_view || active_view->role == WF_VIEW_ROLE_TOPLEVEL)
        last_active_toplevel = active_view;
}

bool wayfire_output::ensure_visible(wayfire_view v)
{
    auto bbox = v->get_bounding_box();
    auto g = this->get_relative_geometry();

    /* Compute the percentage of the view which is visible */
    auto intersection = wf_geometry_intersection(bbox, g);
    double area = 1.0 * intersection.width * intersection.height;
    area /= 1.0 * bbox.width * bbox.height;

    if (area >= 0.1) /* View is somewhat visible, no need for anything special */
        return false;

    /* Otherwise, switch the workspace so the view gets maximum exposure */
    int dx = bbox.x + bbox.width / 2;
    int dy = bbox.y + bbox.height / 2;

    int dvx = std::floor(1.0 * dx / g.width);
    int dvy = std::floor(1.0 * dy / g.height);
    GetTuple(vx, vy, workspace->get_current_workspace());

    change_viewport_signal data;

    data.carried_out = false;
    data.old_viewport = std::make_tuple(vx, vy);
    data.new_viewport = std::make_tuple(vx + dvx, vy + dvy);

    emit_signal("set-workspace-request", &data);
    if (!data.carried_out)
        workspace->set_workspace(data.new_viewport);

    return true;
}

void wayfire_output::focus_view(wayfire_view v, wlr_seat *seat)
{
    if (v && workspace->get_view_layer(v) < core->get_focused_layer())
    {
        log_info("Denying focus request for a view from a lower layer than the focused layer");
        return;
    }

    if (!v || !v->is_mapped())
    {
        /* We can't really focus the view, it isn't mapped or is NULL.
         * But at least we can bring it to front */
        set_active_view(nullptr, seat);
        if (v)
            bring_to_front(v);

        return;
    }

    /* If no keyboard focus surface is set, then we don't want to focus the view */
    if (v->get_keyboard_focus_surface() || interactive_view_from_view(v.get()))
    {
        /* We must make sure the view which gets focus is visible on the
         * current workspace */
        if (v->minimized)
            v->minimize_request(false);

        set_active_view(v, seat);
        bring_to_front(v);

        focus_view_signal data;
        data.view = v;
        emit_signal("focus-view", &data);
    }
}

wayfire_view wayfire_output::get_top_view()
{
    wayfire_view view = nullptr;
    workspace->for_each_view([&view] (wayfire_view v) {
        if (!view)
            view = v;
    }, WF_LAYER_WORKSPACE);

    return view;
}

wayfire_view wayfire_output::get_view_at_point(int x, int y)
{
    wayfire_view chosen = nullptr;

    workspace->for_each_view([x, y, &chosen] (wayfire_view v) {
        if (v->is_visible() && (v->get_wm_geometry() & wf_point{x, y})) {
            if (chosen == nullptr)
                chosen = v;
        }
    }, WF_VISIBLE_LAYERS);

    return chosen;
}

bool wayfire_output::activate_plugin(wayfire_grab_interface owner, bool lower_fs)
{
    if (!owner)
        return false;

    if (core->get_active_output() != this)
        return false;

    if (active_plugins.find(owner) != active_plugins.end())
    {
        log_debug("output %s: activate plugin %s again", handle->name, owner->name.c_str());
        active_plugins.insert(owner);
        return true;
    }

    for(auto act_owner : active_plugins)
    {
        bool compatible = (act_owner->abilities_mask & owner->abilities_mask) == 0;
        if (!compatible)
            return false;
    }

    /* _activation_request is a special signal,
     * used to specify when a plugin is activated. It is used only internally, plugins
     * shouldn't listen for it */
    if (lower_fs && active_plugins.empty())
        emit_signal("_activation_request", (signal_data*)1);

    active_plugins.insert(owner);
    log_debug("output %s: activate plugin %s", handle->name, owner->name.c_str());
    return true;
}

bool wayfire_output::deactivate_plugin(wayfire_grab_interface owner)
{
    auto it = active_plugins.find(owner);
    if (it == active_plugins.end())
        return true;

    active_plugins.erase(it);
    log_debug("output %s: deactivate plugin %s", handle->name, owner->name.c_str());

    if (active_plugins.count(owner) == 0)
    {
        owner->ungrab();
        active_plugins.erase(owner);

        if (active_plugins.empty())
            emit_signal("_activation_request", nullptr);

        return true;
    }


    return false;
}

bool wayfire_output::is_plugin_active(owner_t name)
{
    for (auto act : active_plugins)
        if (act && act->name == name)
            return true;

    return false;
}

wayfire_grab_interface wayfire_output::get_input_grab_interface()
{
    for (auto p : active_plugins)
        if (p && p->is_grabbed())
            return p;

    return nullptr;
}

void wayfire_output::break_active_plugins()
{
    std::vector<wayfire_grab_interface> ifaces;

    for (auto p : active_plugins)
    {
        if (p->callbacks.cancel)
            ifaces.push_back(p);
    }

    for (auto p : ifaces)
        p->callbacks.cancel();
}

/* simple wrappers for core->input, as it isn't exposed to plugins */

wf_binding *wayfire_output::add_key(wf_option key, key_callback *callback)
{
    return core->input->new_binding(WF_BINDING_KEY, key, this, callback);
}

wf_binding *wayfire_output::add_axis(wf_option axis, axis_callback *callback)
{
    return core->input->new_binding(WF_BINDING_AXIS, axis, this, callback);
}

wf_binding *wayfire_output::add_touch(wf_option mod, touch_callback *callback)
{
    return core->input->new_binding(WF_BINDING_TOUCH, mod, this, callback);
}

wf_binding *wayfire_output::add_button(wf_option button,
    button_callback *callback)
{
    return core->input->new_binding(WF_BINDING_BUTTON, button,
        this, callback);
}

wf_binding *wayfire_output::add_gesture(wf_option gesture,
    gesture_callback *callback)
{
    return core->input->new_binding(WF_BINDING_GESTURE, gesture,
        this, callback);
}

wf_binding *wayfire_output::add_activator(wf_option activator,
    activator_callback *callback)
{
    return core->input->new_binding(WF_BINDING_ACTIVATOR, activator,
        this, callback);
}

void wayfire_output::rem_binding(wf_binding *binding)
{
    core->input->rem_binding(binding);
}

void wayfire_output::rem_binding(void *callback)
{
    core->input->rem_binding(callback);
}

uint32_t wf_all_layers_not_below(uint32_t layer)
{
    uint32_t mask = 0;
    for (int i = 0; i < WF_TOTAL_LAYERS; i++)
    {
        if ((1u << i) >= layer)
            mask |= (1 << i);
    }

    return mask;
}
