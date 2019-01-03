#include "debug.hpp"
#include "core.hpp"
#include "opengl.hpp"
#include "output.hpp"
#include "view.hpp"
#include "view-transform.hpp"
#include "decorator.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include "priv-view.hpp"
#include "xdg-shell.hpp"
#include "xdg-shell-v6.hpp"
#include "../output/gtk-shell.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include "signal-definitions.hpp"

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/region.h>
#undef static
}

/* TODO: clean up the code, currently it is a horrible mess
 * Target: split view.cpp into several files, PIMPL the wayfire_view_t and wayfire_surface_t structures */

/* TODO: consistently use wf_geometry/wlr_box, don't simply mix them
 * There must be a distinction(i.e what is a box, what is geometry) */

wayfire_view_t::wayfire_view_t()
    : wayfire_surface_t (NULL)
{
    set_output(core->get_active_output());
}

std::string wayfire_view_t::to_string() const
{
    return "view-" + wf_object_base::to_string();
}

static void handle_toplevel_handle_v1_maximize_request(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_foreign_toplevel_handle_v1_maximized_event*> (data);
    auto view = wf_view_from_void(ev->toplevel->data);

    view->maximize_request(ev->maximized);
}

static void handle_toplevel_handle_v1_minimize_request(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_foreign_toplevel_handle_v1_minimized_event*> (data);
    auto view = wf_view_from_void(ev->toplevel->data);

    view->minimize_request(ev->minimized);
}

static void handle_toplevel_handle_v1_activate_request(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_foreign_toplevel_handle_v1_activated_event*> (data);
    auto view = wf_view_from_void(ev->toplevel->data);

    view->focus_request();
}

static void handle_toplevel_handle_v1_set_rectangle_request(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_foreign_toplevel_handle_v1_set_rectangle_event*> (data);
    auto view = wf_view_from_void(ev->toplevel->data);

    auto surface = wf_surface_from_void(ev->surface->data);
    if (!surface)
    {
        log_error("Setting minimize hint to unknown surface.");
        return;
    }

    if (surface->get_output() != view->get_output())
    {
        log_info("Minimize hint set to surface on a different output, "
            "problems might arise");
    }

    auto box = surface->get_output_geometry();
    box.x += ev->x;
    box.y += ev->y;
    box.width = ev->width;
    box.height = ev->height;

    view->handle_minimize_hint(box);
}

void wayfire_view_t::create_toplevel()
{
    if (toplevel_handle)
        return;

    /* We don't want to create toplevels for shell views or xwayland menus */
    if (role != WF_VIEW_ROLE_TOPLEVEL)
        return;

    toplevel_handle = wlr_foreign_toplevel_handle_v1_create(
        core->protocols.toplevel_manager);
    toplevel_handle->data = this;

    toplevel_handle_v1_maximize_request.notify =
        handle_toplevel_handle_v1_maximize_request;
    toplevel_handle_v1_minimize_request.notify =
        handle_toplevel_handle_v1_minimize_request;
    toplevel_handle_v1_activate_request.notify =
        handle_toplevel_handle_v1_activate_request;
    toplevel_handle_v1_set_rectangle_request.notify =
        handle_toplevel_handle_v1_set_rectangle_request;
    wl_signal_add(&toplevel_handle->events.request_maximize,
        &toplevel_handle_v1_maximize_request);
    wl_signal_add(&toplevel_handle->events.request_minimize,
        &toplevel_handle_v1_minimize_request);
    wl_signal_add(&toplevel_handle->events.request_activate,
        &toplevel_handle_v1_activate_request);
    wl_signal_add(&toplevel_handle->events.set_rectangle,
        &toplevel_handle_v1_set_rectangle_request);

    toplevel_send_title();
    toplevel_send_app_id();
    toplevel_send_state();
    toplevel_update_output(output, true);
}

void wayfire_view_t::destroy_toplevel()
{
    if (!toplevel_handle)
        return;

    toplevel_handle->data = NULL;
    wl_list_remove(&toplevel_handle_v1_maximize_request.link);
    wl_list_remove(&toplevel_handle_v1_activate_request.link);
    wl_list_remove(&toplevel_handle_v1_minimize_request.link);
    wl_list_remove(&toplevel_handle_v1_set_rectangle_request.link);

    wlr_foreign_toplevel_handle_v1_destroy(toplevel_handle);
    toplevel_handle = NULL;
}

void wayfire_view_t::toplevel_send_title()
{
    if (!toplevel_handle)
        return;
    wlr_foreign_toplevel_handle_v1_set_title(toplevel_handle,
        get_title().c_str());
}

void wayfire_view_t::toplevel_send_app_id()
{
    if (!toplevel_handle)
        return;

    std::string app_id;

    auto default_app_id = get_app_id();
    auto gtk_shell_app_id = wf_gtk_shell_get_custom_app_id(
        core->protocols.gtk_shell, surface->resource);

    auto app_id_mode = (*core->config)["workarounds"]
        ->get_option("app_id_mode", "stock");

    if (app_id_mode->as_string() == "gtk-shell" && gtk_shell_app_id.length() > 0) {
        app_id = gtk_shell_app_id;
    } else if (app_id_mode->as_string() == "full") {
        app_id = default_app_id + " " + gtk_shell_app_id;
    } else {
        app_id = default_app_id;
    }

    wlr_foreign_toplevel_handle_v1_set_app_id(toplevel_handle, app_id.c_str());
}

void wayfire_view_t::toplevel_send_state()
{
    if (!toplevel_handle)
        return;

    wlr_foreign_toplevel_handle_v1_set_maximized(toplevel_handle, maximized);
    wlr_foreign_toplevel_handle_v1_set_activated(toplevel_handle, activated);
    wlr_foreign_toplevel_handle_v1_set_minimized(toplevel_handle, minimized);
}

void wayfire_view_t::toplevel_update_output(wayfire_output *wo, bool enter)
{
    if (!wo || !toplevel_handle)
        return;

    if (enter) {
        wlr_foreign_toplevel_handle_v1_output_enter(
            toplevel_handle, wo->handle);
    } else {
        wlr_foreign_toplevel_handle_v1_output_leave(
            toplevel_handle, wo->handle);
    }
}

void wayfire_view_t::handle_title_changed()
{
    title_changed_signal data;
    data.view = self();
    if (output)
        output->emit_signal("view-title-changed", &data);
    emit_signal("title-changed", &data);

    toplevel_send_title();
}

void wayfire_view_t::handle_app_id_changed()
{
    app_id_changed_signal data;
    data.view = self();
    if (output)
        output->emit_signal("view-app-id-changed", &data);
    emit_signal("app-id-changed", &data);

    toplevel_send_app_id();
}

void wayfire_view_t::handle_minimize_hint(const wlr_box &hint)
{
    this->minimize_hint = hint;
}

wlr_box wayfire_view_t::get_minimize_hint()
{
    return minimize_hint;
}

void wayfire_view_t::set_output(wayfire_output *wo)
{
    _output_signal data;
    data.output = output;

    toplevel_update_output(output, false);
    wayfire_surface_t::set_output(wo);
    if (decoration)
        decoration->set_output(wo);

    toplevel_update_output(wo, true);
    if (wo != data.output)
        emit_signal("set-output", &data);
}

void wayfire_view_t::set_role(wf_view_role new_role)
{
    if (new_role != WF_VIEW_ROLE_TOPLEVEL)
        destroy_toplevel();
    role = new_role;
}

wayfire_view wayfire_view_t::self()
{
    return wayfire_view(this);
}

bool wayfire_view_t::is_visible()
{
    return (is_mapped() || offscreen_buffer.valid()) && !is_hidden;
}

void wayfire_view_t::adjust_anchored_edge(int32_t new_width, int32_t new_height)
{
    if (edges)
    {
        auto wm = get_wm_geometry();
        if (edges & WLR_EDGE_LEFT)
            wm.x += geometry.width - new_width;
        if (edges & WLR_EDGE_TOP)
            wm.y += geometry.height - new_height;

        move(wm.x, wm.y, false);
    }
}

bool wayfire_view_t::update_size()
{
    assert(surface);

    int old_w = geometry.width, old_h = geometry.height;
    int width = surface->current.width, height = surface->current.height;
    if (geometry.width != width || geometry.height != height)
    {
        damage_raw(last_bounding_box);
        adjust_anchored_edge(width, height);
        wayfire_view_t::resize(width, height, true);
    }

    return geometry.width != old_w || geometry.height != old_h;
}

void wayfire_view_t::set_moving(bool moving)
{
    in_continuous_move += moving ? 1 : -1;
}

void wayfire_view_t::set_resizing(bool resizing, uint32_t edges)
{
    /* edges are reset on the next commit */
    if (resizing)
        this->edges = edges;

    in_continuous_resize += resizing ? 1 : -1;
}

void wayfire_view_t::move(int x, int y, bool send_signal)
{
    auto opos = get_output_position();
    auto wm   = get_wm_geometry();

    view_geometry_changed_signal data;
    data.view = self();
    data.old_geometry = wm;

    damage(last_bounding_box);
    geometry.x = x + opos.x - wm.x;
    geometry.y = y + opos.y - wm.y;

    /* Make sure that if we move the view while it is unmapped, its snapshot
     * is still valid coordinates */
    if (offscreen_buffer.valid())
    {
        offscreen_buffer.geometry.x += x - data.old_geometry.x;
        offscreen_buffer.geometry.y += y - data.old_geometry.y;
    }

    damage();

    if (send_signal)
    {
        output->emit_signal("view-geometry-changed", &data);
        emit_signal("geometry-changed", &data);
    }

    last_bounding_box = get_bounding_box();
}

void wayfire_view_t::resize(int w, int h, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = self();
    data.old_geometry = get_wm_geometry();

    damage();
    geometry.width = w;
    geometry.height = h;
    damage();

    if (send_signal)
    {
        output->emit_signal("view-geometry-changed", &data);
        emit_signal("geometry-changed", &data);
    }
}

wayfire_surface_t *wayfire_view_t::map_input_coordinates(int cx, int cy, int& sx, int& sy)
{
    if (!_is_mapped || !is_mapped())
        return nullptr;

    wayfire_surface_t *ret = NULL;

    if (transforms.size())
    {
        auto it = transforms.rbegin();
        auto box = get_untransformed_bounding_box();
        while(it != transforms.rend())
        {
            auto p = (*it)->transform->transformed_to_local_point(box, {cx, cy});
            cx = p.x; cy = p.y;
            ++it;
        }
    }

    for_each_surface(
        [&] (wayfire_surface_t *surface, int x, int y)
        {
            if (ret) return;

            sx = cx - x;
            sy = cy - y;

            if (surface->accepts_input(sx, sy))
                ret = surface;
        });

    return ret;
}

void wayfire_view_t::subtract_opaque(wf_region& region, int x, int y)
{
    int saved_shrink_constraint = maximal_shrink_constraint;

    /* Fullscreen views take up the whole screen, so plugins can't request
     * padding for them (nothing below is visible) */
    if (this->fullscreen)
        maximal_shrink_constraint = 0;

    wayfire_surface_t::subtract_opaque(region, x, y);
    maximal_shrink_constraint = saved_shrink_constraint;
}

wlr_surface *wayfire_view_t::get_keyboard_focus_surface()
{
    if (_is_mapped)
        return surface;

    return NULL;
}

void wayfire_view_t::set_geometry(wf_geometry g)
{
    move(g.x, g.y, false);
    resize(g.width, g.height);
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region,
    nonstd::observer_ptr<wf_view_transformer_t> upto)
{
    auto box = region;
    auto view = get_untransformed_bounding_box();

    for (auto& tr : transforms)
    {
        if (tr->transform.get() == upto.get())
            break;

        box = tr->transform->get_bounding_box(view, box);
        view = tr->transform->get_bounding_box(view, view);
    }

    return box;
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region,
    std::string transformer)
{
    return transform_region(region, get_transformer(transformer));
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region)
{
    return transform_region(region,
        nonstd::observer_ptr<wf_view_transformer_t>(nullptr));
}

bool wayfire_view_t::intersects_region(const wlr_box& region)
{
    /* fallback to the whole transformed boundingbox, if it exists */
    if (!is_mapped())
        return region & get_bounding_box();

    bool result = false;
    for_each_surface([=, &result] (wayfire_surface_t* surface, int, int)
    {
        auto box = transform_region(surface->get_output_geometry());
        result = result || (region & box);
    });

    return result;
}

wf_geometry wayfire_view_t::get_untransformed_bounding_box()
{
    if (is_mapped() || !offscreen_buffer.valid())
    {
        auto bbox = get_output_geometry();
        int x1 = bbox.x, x2 = bbox.x + bbox.width;
        int y1 = bbox.y, y2 = bbox.y + bbox.height;

        for_each_surface([&x1, &x2, &y1, &y2] (wayfire_surface_t* surface, int, int)
                         {
                             auto sbox = surface->get_output_geometry();
                             int sx1 = sbox.x, sx2 = sbox.x + sbox.width;
                             int sy1 = sbox.y, sy2 = sbox.y + sbox.height;

                             x1 = std::min(x1, sx1);
                             y1 = std::min(y1, sy1);
                             x2 = std::max(x2, sx2);
                             y2 = std::max(y2, sy2);
                         });

        bbox.x = x1;
        bbox.y = y1;
        bbox.width = x2 - x1;
        bbox.height = y2 - y1;

        return bbox;
    }

    return offscreen_buffer.geometry;
}

wf_geometry wayfire_view_t::get_bounding_box()
{
    return transform_region(get_untransformed_bounding_box());
}

void wayfire_view_t::set_maximized(bool maxim)
{
    maximized = maxim;

    if (frame)
        frame->notify_view_maximized();

    toplevel_send_state();
}

void wayfire_view_t::set_minimized(bool minim)
{
    minimized = minim;
    if (minimized)
    {
        view_disappeared_signal data;
        data.view = self();
        emit_signal("disappear", &data);

        output->emit_signal("view-disappeared", &data);
        output->workspace->add_view_to_layer(self(), WF_LAYER_MINIMIZED);

        /* We want to be sure that when we restore the view, it will be visible
         * on the then current workspace
         *
         * Because the minimized layer doesn't move when switching workspaces,
         * we know that making it "visible" in the minimize layer will ensure
         * it is visible when we restore it */
        auto bbox = get_bounding_box();
        auto workspace = output->get_relative_geometry();

        if (!(bbox & workspace))
        {
            /* Make the center of the view on the current workspace */
            int cx = bbox.x + bbox.width / 2;
            int cy = bbox.y + bbox.height / 2;

            int width = workspace.width, height = workspace.height;
            /* compute center coordinates when moved to the current workspace */
            int local_cx = (cx % width + width) % width;
            int local_cy = (cy % height + height) % height;

            auto wm_geometry = get_wm_geometry();
            move(wm_geometry.x + local_cx - cx, wm_geometry.y + local_cy - cy);
        }
    } else
    {
        output->workspace->add_view_to_layer(self(), WF_LAYER_WORKSPACE);
        output->focus_view(self());
    }

    toplevel_send_state();
}

void wayfire_view_t::set_fullscreen(bool full)
{
    fullscreen = full;

    if (frame)
        frame->notify_view_fullscreened();

    if (fullscreen && output)
    {
        if (saved_layer == 0)
            saved_layer = output->workspace->get_view_layer(self());

        /* Will trigger raising to fullscreen layer in workspace-manager */
        output->bring_to_front(self());
    }

    if (!fullscreen && output &&
        output->workspace->get_view_layer(self()) == WF_LAYER_FULLSCREEN)
    {
        output->workspace->add_view_to_layer(self(),
            saved_layer == 0 ? (uint32_t)WF_LAYER_WORKSPACE : saved_layer);
        saved_layer = 0;
    }

    toplevel_send_state();
}

void wayfire_view_t::activate(bool active)
{
    if (frame)
        frame->notify_view_activated(active);

    /* we don't send activated or deactivated for shell views,
     * they should always be active */
    if (role == WF_VIEW_ROLE_SHELL_VIEW)
    {
        if (!active)
            return activate(true);
    }

    activated = active;
    toplevel_send_state();
}

void wayfire_view_t::close()
{
}

void wayfire_view_t::set_parent(wayfire_view parent)
{
    if (this->parent)
    {
        auto it = std::remove(this->parent->children.begin(), this->parent->children.end(), self());
        this->parent->children.erase(it);
    }

    this->parent = parent;
    if (parent)
    {
        auto it = std::find(parent->children.begin(), parent->children.end(), self());
        if (it == parent->children.end())
            parent->children.push_back(self());
    }
}

wayfire_surface_t* wayfire_view_t::get_main_surface()
{
    return this;
}

void wayfire_view_t::for_each_surface(wf_surface_iterator_callback callback, bool reverse)
{
    if (reverse && decoration)
    {
        auto pos = decoration->get_output_position();
        callback(decoration, pos.x, pos.y);
    }

    wayfire_surface_t::for_each_surface(callback, reverse);

    if (!reverse && decoration)
    {
        auto pos = decoration->get_output_position();
        callback(decoration, pos.x, pos.y);
    }
}

bool wayfire_view_t::should_be_decorated()
{
    return role == WF_VIEW_ROLE_TOPLEVEL && !has_client_decoration;
}

void wayfire_view_t::set_decoration(wayfire_surface_t *deco)
{
    damage();

    if (decoration)
        decoration->dec_keep_count();

    auto wm = get_wm_geometry();

    decoration = deco;
    frame = dynamic_cast<wf_decorator_frame_t*> (deco);

    if (!deco)
        return;

    assert(frame);
    decoration->parent_surface = this;
    decoration->set_output(output);
    decoration->inc_keep_count();

    frame->notify_view_resized(frame->expand_wm_geometry(wm));
    move(wm.x, wm.y);

    damage();

    if (maximized)
        set_geometry(output->workspace->get_workarea());
    if (fullscreen)
        set_geometry(output->get_relative_geometry());
}

wf_point wayfire_view_t::get_output_position()
{
    return wf_point{geometry.x, geometry.y};
}

wf_geometry wayfire_view_t::get_wm_geometry()
{
    if (frame)
        return frame->expand_wm_geometry(geometry);
    else
        return geometry;
}

void wayfire_view_t::damage_raw(const wlr_box& box)
{
    auto damage_box = output->render->get_target_framebuffer().
        damage_box_from_geometry_box(box);

    /* shell views are visible in all workspaces. That's why we must apply
     * their damage to all workspaces as well */
    if (role == WF_VIEW_ROLE_SHELL_VIEW)
    {
        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        GetTuple(vx, vy, output->workspace->get_current_workspace());

        /* Damage only the visible region of the shell view.
         * This prevents hidden panels from spilling damage onto other workspaces */
        wlr_box ws_box = output->render->get_damage_box();
        wlr_box visible_damage = wf_geometry_intersection(damage_box, ws_box);

        for (int i = 0; i < vw; i++)
        {
            for (int j = 0; j < vh; j++)
            {
                const int dx = (i - vx) * ws_box.width;
                const int dy = (j - vy) * ws_box.height;
                output->render->damage(visible_damage + wf_point{dx, dy});
            }
        }
    } else
    {
        output->render->damage(damage_box);
    }

    emit_signal("damaged-region", nullptr);
}

void wayfire_view_t::damage(const wlr_box& box)
{
    if (!output)
        return;

    if (!has_transformer())
        return damage_raw(box);

    auto wm_geometry = get_wm_geometry();

    auto real_box = box;
    real_box.x -= wm_geometry.x;
    real_box.y -= wm_geometry.y;

    offscreen_buffer.cached_damage |= real_box;
    damage_raw(transform_region(box));
}

bool wayfire_view_t::offscreen_buffer_t::valid()
{
    return fb != (uint32_t)-1;
}

bool wayfire_view_t::can_take_snapshot()
{
    return get_buffer();
}

void wayfire_view_t::take_snapshot()
{
    if (!can_take_snapshot())
        return;

    auto buffer_geometry = get_untransformed_bounding_box();
    offscreen_buffer.geometry = buffer_geometry;

    float scale = output->handle->scale;
    if (int(buffer_geometry.width  * scale) != offscreen_buffer.viewport_width ||
        int(buffer_geometry.height * scale) != offscreen_buffer.viewport_height ||
        offscreen_buffer.scale != scale)
    {
        // scale/size changed, invalidate offscreen buffer
        last_offscreen_buffer_age = -1;
    }

    /* Nothing has changed, the last buffer is still valid */
    if (buffer_age <= last_offscreen_buffer_age)
        return;

    last_offscreen_buffer_age = buffer_age;

    OpenGL::render_begin();
    offscreen_buffer.allocate(buffer_geometry.width * scale, buffer_geometry.height * scale);
    offscreen_buffer.scale = scale;
    offscreen_buffer.bind();
    OpenGL::clear({0, 0, 0, 0});
    OpenGL::render_end();

    wf_region full_region{{0, 0, offscreen_buffer.viewport_width,
        offscreen_buffer.viewport_height}};
    for_each_surface([=] (wayfire_surface_t *surface, int x, int y)
    {
        surface->simple_render(offscreen_buffer,
            x - buffer_geometry.x, y - buffer_geometry.y, full_region);
    }, true);
}

void wayfire_view_t::render_fb(const wf_region& damaged_region, const wf_framebuffer& fb)
{
    in_paint = true;
    if (has_transformer())
    {
        take_snapshot();

        wf_geometry obox = get_untransformed_bounding_box();
        obox.width = offscreen_buffer.geometry.width;
        obox.height = offscreen_buffer.geometry.height;

        auto it = transforms.begin();

        GLuint last_tex = offscreen_buffer.tex;
        while(std::next(it) != transforms.end())
        {
            auto& tr = *it;
            auto& fb = tr->fb;

            auto tbox = tr->transform->get_bounding_box(obox, obox);

            OpenGL::render_begin();
            fb.allocate(tbox.width, tbox.height); // resize if needed
            fb.geometry = tbox;
            fb.bind(); // bind buffer to clear
            OpenGL::clear({0, 0, 0, 0});
            OpenGL::render_end();

            wf_region whole_region{wlr_box{0, 0, tbox.width, tbox.height}};
            tr->transform->render_with_damage(last_tex, obox, whole_region, fb);
            last_tex = fb.tex;

            obox = tbox;
            ++it;
        }

        (*it)->transform->render_with_damage(last_tex, obox, damaged_region, fb);
        cleanup_transforms();
    } else
    {
        wayfire_surface_t::render_fb(damaged_region, fb);
    }
    in_paint = false;
}

void wayfire_view_t::add_transformer(std::unique_ptr<wf_view_transformer_t> transformer, std::string name)
{
    damage();
    auto tr = nonstd::make_unique<transform_t> ();
    tr->transform = std::move(transformer);
    tr->plugin_name = name;

    auto it = transforms.begin();
    while (it != transforms.end())
    {
        if ((*it)->transform->get_z_order() >= tr->transform->get_z_order()) {
            break;
        } else {
            ++it;
        }
    }

    transforms.insert(it, std::move(tr));
    damage();
}

void wayfire_view_t::add_transformer(std::unique_ptr<wf_view_transformer_t> transformer)
{

    add_transformer(std::move(transformer), "");
}

nonstd::observer_ptr<wf_view_transformer_t> wayfire_view_t::get_transformer(std::string name)
{
    for (auto& tr : transforms)
    {
        if (tr->plugin_name == name)
            return nonstd::make_observer(tr->transform.get());
    }

    return nullptr;
}

void wayfire_view_t::_pop_transformer(nonstd::observer_ptr<transform_t> transformer)
{
    damage();

    auto it = transforms.begin();
    while(it != transforms.end())
    {
        if ((*it).get() == transformer.get())
        {
            (*it)->fb.release();
            it = transforms.erase(it);
        } else
        {
            ++it;
        }
    }

    damage();
}

void wayfire_view_t::pop_transformer(nonstd::observer_ptr<wf_view_transformer_t> transformer)
{
    for(auto& tr : transforms)
    {
        if (tr->transform.get() == transformer.get())
            tr->to_remove = 1;
    }

    if (!in_paint)
        cleanup_transforms();
}

void wayfire_view_t::pop_transformer(std::string name)
{
    for(auto& tr : transforms)
    {
        if (tr->plugin_name == name)
            tr->to_remove = 1;
    }

    if (!in_paint)
        cleanup_transforms();
}

void wayfire_view_t::cleanup_transforms()
{
    std::vector<nonstd::observer_ptr<transform_t>> to_remove;
    for (auto& tr: transforms)
    {
        if (tr->to_remove)
            to_remove.push_back(nonstd::make_observer(tr.get()));
    }

    for (auto r : to_remove)
        _pop_transformer(r);
}

bool wayfire_view_t::has_transformer()
{
    return transforms.size();
}

wlr_box wayfire_view_t::get_bounding_box(std::string transformer)
{
    return get_bounding_box(get_transformer(transformer));
}

wlr_box wayfire_view_t::get_bounding_box(
    nonstd::observer_ptr<wf_view_transformer_t> transformer)
{
    return transform_region(get_untransformed_bounding_box(), transformer);
}


void emit_view_map(wayfire_view view)
{
    /* TODO: consider not emitting a create-view for special surfaces */
    map_view_signal data;
    data.view = view;
    view->get_output()->emit_signal("map-view", &data);
    view->emit_signal("map", &data);
    emit_map_state_change(view.get());
}

void wayfire_view_t::reposition_relative_to_parent()
{
    assert(parent);
    auto workarea = output->workspace->get_workarea();

    auto wm_geometry = get_wm_geometry();
    if (parent->is_mapped())
    {
        auto parent_geometry = parent->get_wm_geometry();
        int sx = parent_geometry.x + (parent_geometry.width  - wm_geometry.width) / 2;
        int sy = parent_geometry.y + (parent_geometry.height - wm_geometry.height) / 2;

        move(sx, sy, false);
    } else
    {
        /* if we have a parent which still isn't mapped, we cannot determine
         * the view's position, so we center it on the screen */
        int sx = workarea.width / 2 - wm_geometry.width / 2;
        int sy = workarea.height/ 2 - wm_geometry.height/ 2;

        move(sx, sy, false);
    }
}

void wayfire_view_t::map(wlr_surface *surface)
{
    _is_mapped = true;
    wayfire_surface_t::map(surface);

    if (role == WF_VIEW_ROLE_TOPLEVEL && !parent && !maximized && !fullscreen)
    {
        auto wm = get_wm_geometry();
        auto workarea = output->workspace->get_workarea();

        move(wm.x + workarea.x, wm.y + workarea.y, false);
    }

    if (update_size())
        damage();

    if (parent)
        reposition_relative_to_parent();

    if (role != WF_VIEW_ROLE_SHELL_VIEW)
    {
        output->attach_view(self());
        output->focus_view(self());
    }

    emit_view_map(self());
}

void emit_view_unmap(wayfire_view view)
{
    unmap_view_signal data;
    data.view = view;

    view->get_output()->emit_signal("unmap-view", &data);
    view->get_output()->emit_signal("view-disappeared", &data);
    view->emit_signal("unmap", &data);
    view->emit_signal("disappeared", &data);
}

void wayfire_view_t::unmap()
{
    _is_mapped = false;
    destroy_toplevel();

    if (parent)
        set_toplevel_parent(nullptr);

    auto copy = children;
    for (auto c : copy)
        c->set_toplevel_parent(nullptr);

    log_info("unmap %s %s %p", get_title().c_str(), get_app_id().c_str(), this);

    if (output)
        emit_view_unmap(self());

    wayfire_surface_t::unmap();
}

void wayfire_view_t::move_request()
{
    move_request_signal data;
    data.view = self();
    output->emit_signal("move-request", &data);
}

void wayfire_view_t::focus_request()
{
    core->focus_view(self(), nullptr);
    output->ensure_visible(self());
}

void wayfire_view_t::resize_request(uint32_t edges)
{
    resize_request_signal data;
    data.view = self();
    data.edges = edges;
    output->emit_signal("resize-request", &data);
}

void wayfire_view_t::maximize_request(bool state)
{
    if (maximized == state)
        return;

    view_maximized_signal data;
    data.view = self();
    data.state = state;

    if (surface)
    {
        output->emit_signal("view-maximized-request", &data);
    } else if (state)
    {
        set_geometry(output->workspace->get_workarea());
        set_maximized(state);
        output->emit_signal("view-maximized", &data);
    }
}

void wayfire_view_t::minimize_request(bool state)
{
    if (state == minimized || !is_mapped())
        return;

    view_minimize_request_signal data;
    data.view = self();
    data.state = state;

    if (is_mapped())
    {
        output->emit_signal("view-minimize-request", &data);
        /* Some plugin (e.g animate) will take care of the request, so we need
         * to just send proper state to foreign-toplevel clients */
        if (data.carried_out)
        {
            minimized = state;
            toplevel_send_state();

        } else
        {
            /* Do the default minimization */
            set_minimized(state);
        }
    }
}

void wayfire_view_t::fullscreen_request(wayfire_output *out, bool state)
{
    if (fullscreen == state)
        return;

    auto wo = (out ? out : (output ? output : core->get_active_output()));
    assert(wo);

    if (output != wo)
        core->move_view_to_output(self(), wo);

    view_fullscreen_signal data;
    data.view = self();
    data.state = state;

    if (surface) {
        wo->emit_signal("view-fullscreen-request", &data);
    } else if (state) {
        set_geometry(output->get_relative_geometry());
        output->emit_signal("view-fullscreen", &data);
    }

    set_fullscreen(state);
}

void wayfire_view_t::commit()
{
    wayfire_surface_t::commit();
    if (update_size())
    {
        if (frame)
            frame->notify_view_resized(get_wm_geometry());
    }

    /* clear the resize edges.
     * This is must be done here because if the user(or plugin) resizes too fast,
     * the shell client might still haven't configured the surface, * and in this
     * case the next commit(here) needs to still have access to the gravity */
    if (!in_continuous_resize)
        edges = 0;

    this->last_bounding_box = get_bounding_box();
}

void wayfire_view_t::damage()
{
    damage(get_untransformed_bounding_box());
}

void wayfire_view_t::destruct()
{
    set_decoration(nullptr);
    core->erase_view(self());
}

void wayfire_view_t::destroy()
{
    destroyed = 1;
    dec_keep_count();
}

void wayfire_view_t::set_toplevel_parent(wayfire_view new_parent)
{
    /* Nothing changed */
    if (new_parent == parent)
        return;

    /* Erase from the old parent */
    if (parent)
    {
        auto it = std::remove(this->parent->children.begin(), this->parent->children.end(), self());
        this->parent->children.erase(it, this->parent->children.end());
    }

    /* Add in the list of the new parent */
    if (new_parent)
        new_parent->children.push_back(self());

    parent = new_parent;

    /* if the view isn't mapped, then it will be positioned properly in map() */
    if (is_mapped())
    {
        if (parent)
            reposition_relative_to_parent();
    }
}

wayfire_view_t::~wayfire_view_t()
{
}

void init_desktop_apis()
{
    init_xdg_shell();
    init_layer_shell();
    init_xdg_shell_v6();
    init_xwayland();
}
