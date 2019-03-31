#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "decorator.hpp"
#include "xdg-shell.hpp"

/* TODO: Figure out a way to animate this */
wayfire_xdg_popup::wayfire_xdg_popup(wlr_xdg_popup *popup)
    :wayfire_surface_t(wf_surface_from_void(popup->parent->data))
{
    assert(parent_surface);
    this->popup = popup;

    on_map.set_callback([&] (void*) { map(this->popup->base->surface); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroyed = 1; dec_keep_count(); });
    on_new_popup.set_callback([&] (void* data) { create_xdg_popup((wlr_xdg_popup*) data); });

    on_map.connect(&popup->base->events.map);
    on_unmap.connect(&popup->base->events.unmap);
    on_destroy.connect(&popup->base->events.destroy);
    on_new_popup.connect(&popup->base->events.new_popup);

    popup->base->data = this;
    unconstrain();
}

void wayfire_xdg_popup::unconstrain()
{
    auto view = dynamic_cast<wayfire_view_t*> (get_main_surface());
    if (!output || !view)
        return;

    auto box = output->get_relative_geometry();
    auto wm = view->get_output_geometry();
    box.x -= wm.x;
    box.y -= wm.y;

    wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

wayfire_xdg_popup::~wayfire_xdg_popup()
{
    on_map.disconnect();
    on_unmap.disconnect();
    on_destroy.disconnect();
    on_new_popup.disconnect();
}

void wayfire_xdg_popup::get_child_position(int &x, int &y)
{
    auto parent = wf_surface_from_void(popup->parent->data);
    assert(parent);

    parent->get_child_offset(x, y);
    x += popup->geometry.x - popup->base->geometry.x;
    y += popup->geometry.y - popup->base->geometry.y;
}

void wayfire_xdg_popup::get_child_offset(int &x, int &y)
{
    x = popup->base->geometry.x;
    y = popup->base->geometry.y;
}

void wayfire_xdg_popup::send_done()
{
    if (is_mapped())
        wlr_xdg_popup_destroy(popup->base);
}

void create_xdg_popup(wlr_xdg_popup *popup)
{
    auto parent = wf_surface_from_void(popup->parent->data);
    if (!parent)
    {
        log_error("attempting to create a popup with unknown parent");
        return;
    }

    new wayfire_xdg_popup(popup);
}

wayfire_xdg_view::wayfire_xdg_view(wlr_xdg_surface *s)
    : wayfire_view_t(), xdg_surface(s)
{
    log_info ("new xdg_shell_stable surface: %s app-id: %s",
              nonull(xdg_surface->toplevel->title),
              nonull(xdg_surface->toplevel->app_id));

    on_map.set_callback([&] (void*) { map(this->xdg_surface->surface); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroyed = 1; dec_keep_count(); });
    on_new_popup.set_callback([&] (void* data) { create_xdg_popup((wlr_xdg_popup*) data); });
    on_set_title.set_callback([&] (void*) { handle_title_changed(); });
    on_set_app_id.set_callback([&] (void*) { handle_app_id_changed(); });
    on_set_parent.set_callback([&] (void*) {
        auto parent = xdg_surface->toplevel->parent ?
            wf_view_from_void(xdg_surface->toplevel->parent->data)->self() : nullptr;
        set_toplevel_parent(parent);
    });

    on_request_move.set_callback([&] (void*) { move_request(); });
    on_request_resize.set_callback([&] (void*) { resize_request(); });
    on_request_minimize.set_callback([&] (void*) { minimize_request(true); });
    on_request_maximize.set_callback([&] (void* data) {
        maximize_request(xdg_surface->toplevel->client_pending.maximized);
    });
    on_request_fullscreen.set_callback([&] (void* data) {
        auto ev = static_cast<wlr_xdg_toplevel_set_fullscreen_event*> (data);
        auto wo = core->get_output(ev->output);
        fullscreen_request(wo, ev->fullscreen);
    });

    on_map.connect(&xdg_surface->events.map);
    on_unmap.connect(&xdg_surface->events.unmap);
    on_destroy.connect(&xdg_surface->events.destroy);
    on_new_popup.connect(&xdg_surface->events.new_popup);

    on_set_title.connect(&xdg_surface->toplevel->events.set_title);
    on_set_app_id.connect(&xdg_surface->toplevel->events.set_app_id);
    on_set_parent.connect(&xdg_surface->toplevel->events.set_parent);
    on_request_move.connect(&xdg_surface->toplevel->events.request_move);
    on_request_resize.connect(&xdg_surface->toplevel->events.request_resize);
    on_request_maximize.connect(&xdg_surface->toplevel->events.request_maximize);
    on_request_minimize.connect(&xdg_surface->toplevel->events.request_minimize);
    on_request_fullscreen.connect(&xdg_surface->toplevel->events.request_fullscreen);

    wlr_xdg_surface_ping(s);
    xdg_surface->data = this;
}

static wf_geometry get_xdg_geometry(wlr_xdg_surface *surface)
{
    wlr_box xdg_geometry;
    wlr_xdg_surface_get_geometry(surface, &xdg_geometry);
    return xdg_geometry;
}

void wayfire_xdg_view::on_xdg_geometry_updated()
{
    auto wm = get_wm_geometry();
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    xdg_surface_offset = {xdg_geometry.x, xdg_geometry.y};
    move(wm.x, wm.y, false);
}

void wayfire_xdg_view::commit()
{
    wayfire_view_t::commit();

    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    if (xdg_geometry.x != xdg_surface_offset.x ||
        xdg_geometry.y != xdg_surface_offset.y)
    {
        on_xdg_geometry_updated();
    }
}

void wayfire_xdg_view::map(wlr_surface *surface)
{
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    xdg_surface_offset = {xdg_geometry.x, xdg_geometry.y};

    if (xdg_surface->toplevel->client_pending.maximized)
        maximize_request(true);

    if (xdg_surface->toplevel->client_pending.fullscreen)
        fullscreen_request(output, true);

    if (xdg_surface->toplevel->parent)
    {
        auto parent = wf_view_from_void(xdg_surface->toplevel->parent->data)->self();
        set_toplevel_parent(parent);
    }

    wayfire_view_t::map(surface);
    create_toplevel();
}

void wayfire_xdg_view::get_child_offset(int &x, int &y)
{
    x = xdg_surface_offset.x;
    y = xdg_surface_offset.y;
}

wf_geometry wayfire_xdg_view::get_wm_geometry()
{
    if (!xdg_surface)
        return get_untransformed_bounding_box();

    auto opos = get_output_position();
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    wf_geometry wm = {
        opos.x + xdg_surface_offset.x,
        opos.y + xdg_surface_offset.y,
        xdg_geometry.width,
        xdg_geometry.height
    };

    if (frame)
        wm = frame->expand_wm_geometry(wm);

    return wm;
}

void wayfire_xdg_view::activate(bool act)
{
    /* we don't send activated or deactivated for shell views,
     * they should always be active */
    if (this->role == WF_VIEW_ROLE_SHELL_VIEW)
        act = true;

    wlr_xdg_toplevel_set_activated(xdg_surface, act);
    wayfire_view_t::activate(act);
}

void wayfire_xdg_view::set_tiled(uint32_t edges)
{
    wlr_xdg_toplevel_set_tiled(xdg_surface, edges);
    this->tiled_edges = edges;
}

void wayfire_xdg_view::set_maximized(bool max)
{
    wayfire_view_t::set_maximized(max);
    wlr_xdg_toplevel_set_maximized(xdg_surface, max);
}

void wayfire_xdg_view::set_fullscreen(bool full)
{
    wayfire_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_set_fullscreen(xdg_surface, full);
}

void wayfire_xdg_view::move(int w, int h, bool send)
{
    wayfire_view_t::move(w, h, send);
}

void wayfire_xdg_view::resize(int w, int h, bool send)
{
    damage();

    if (frame)
        frame->calculate_resize_size(w, h);

    wlr_xdg_toplevel_set_size(xdg_surface, w, h);
}

void wayfire_xdg_view::request_native_size()
{
    wlr_xdg_toplevel_set_size(xdg_surface, 0, 0);
}

std::string wayfire_xdg_view::get_app_id()
{
    return nonull(xdg_surface->toplevel->app_id);
}

std::string wayfire_xdg_view::get_title()
{
    return nonull(xdg_surface->toplevel->title);
}

void wayfire_xdg_view::close()
{
    wlr_xdg_toplevel_send_close(xdg_surface);
    wayfire_view_t::close();
}

void wayfire_xdg_view::destroy()
{
    on_map.disconnect();
    on_unmap.disconnect();
    on_destroy.disconnect();
    on_new_popup.disconnect();
    on_set_title.disconnect();
    on_set_app_id.disconnect();
    on_set_parent.disconnect();
    on_request_move.disconnect();
    on_request_resize.disconnect();
    on_request_maximize.disconnect();
    on_request_minimize.disconnect();
    on_request_fullscreen.disconnect();

    xdg_surface = nullptr;
    wayfire_view_t::destroy();
}

wayfire_xdg_view::~wayfire_xdg_view() {}

static wlr_xdg_shell *xdg_handle;
void init_xdg_shell()
{
    static wf::wl_listener_wrapper on_created;
    xdg_handle = wlr_xdg_shell_create(core->display);
    if (xdg_handle)
    {
        on_created.set_callback([&] (void *data) {
            auto surf = static_cast<wlr_xdg_surface*> (data);
            if (surf->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
                core->add_view(std::make_unique<wayfire_xdg_view> (surf));
        });
        on_created.connect(&xdg_handle->events.new_surface);
    }
}
