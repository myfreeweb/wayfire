#include "priv-view.hpp"
#include "xdg-shell-v6.hpp"
#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "decorator.hpp"
#include "output-layout.hpp"

static void create_xdg6_popup(wlr_xdg_popup_v6* popup);

/* TODO: Figure out a way to animate this */
wayfire_xdg6_popup::wayfire_xdg6_popup(wlr_xdg_popup_v6 *popup)
    :wayfire_surface_t(wf_surface_from_void(popup->parent->surface->data))
{
    assert(parent_surface);
    this->popup = popup;

    on_map.set_callback([&] (void*) { map(this->popup->base->surface); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroyed = 1; dec_keep_count(); });
    on_new_popup.set_callback([&] (void* data) { create_xdg6_popup((wlr_xdg_popup_v6*) data); });

    on_map.connect(&popup->base->events.map);
    on_unmap.connect(&popup->base->events.unmap);
    on_destroy.connect(&popup->base->events.destroy);
    on_new_popup.connect(&popup->base->events.new_popup);

    popup->base->data = this;
    unconstrain();
}

void wayfire_xdg6_popup::unconstrain()
{
    auto view = dynamic_cast<wayfire_view_t*> (get_main_surface());
    if (!output || !view)
        return;

    auto box = output->get_relative_geometry();
    auto wm = view->get_output_geometry();
    box.x -= wm.x;
    box.y -= wm.y;

    wlr_xdg_popup_v6_unconstrain_from_box(popup, &box);
}

wayfire_xdg6_popup::~wayfire_xdg6_popup()
{
    on_map.disconnect();
    on_unmap.disconnect();
    on_destroy.disconnect();
    on_new_popup.disconnect();
}

void wayfire_xdg6_popup::get_child_position(int &x, int &y)
{
    auto parent = wf_surface_from_void(popup->parent->data);
    parent->get_child_offset(x, y);

    x += popup->geometry.x - popup->base->geometry.x;
    y += popup->geometry.y - popup->base->geometry.y;
}

void wayfire_xdg6_popup::send_done()
{
    wlr_xdg_surface_v6_send_close(popup->base);
}

static void create_xdg6_popup(wlr_xdg_popup_v6 *popup)
{
    auto parent = wf_surface_from_void(popup->parent->surface->data);
    if (!parent)
    {
        log_error("attempting to create a popup with unknown parent");
        return;
    }

    new wayfire_xdg6_popup(popup);
}

wayfire_xdg6_view::wayfire_xdg6_view(wlr_xdg_surface_v6 *s)
    : wayfire_view_t(), v6_surface(s)
{
    log_info ("new xdg_shell_v6 surface: %s app-id: %s",
              nonull(v6_surface->toplevel->title),
              nonull(v6_surface->toplevel->app_id));

    on_map.set_callback([&] (void*) { map(this->v6_surface->surface); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroyed = 1; dec_keep_count(); });
    on_new_popup.set_callback([&] (void* data) { create_xdg6_popup((wlr_xdg_popup_v6*) data); });
    on_set_title.set_callback([&] (void*) { handle_title_changed(); });
    on_set_app_id.set_callback([&] (void*) { handle_app_id_changed(); });
    on_set_parent.set_callback([&] (void*) {
        auto parent = v6_surface->toplevel->parent ?
            wf_view_from_void(v6_surface->toplevel->parent->data)->self() : nullptr;
        set_toplevel_parent(parent);
    });

    on_request_move.set_callback([&] (void*) { move_request(); });
    on_request_resize.set_callback([&] (void*) { resize_request(); });
    on_request_minimize.set_callback([&] (void*) { minimize_request(true); });
    on_request_maximize.set_callback([&] (void* data) {
        maximize_request(v6_surface->toplevel->client_pending.maximized);
    });
    on_request_fullscreen.set_callback([&] (void* data) {
        auto ev = static_cast<wlr_xdg_toplevel_v6_set_fullscreen_event*> (data);
        auto wo = core->output_layout->find_output(ev->output);
        fullscreen_request(wo, ev->fullscreen);
    });

    on_map.connect(&v6_surface->events.map);
    on_unmap.connect(&v6_surface->events.unmap);
    on_destroy.connect(&v6_surface->events.destroy);
    on_new_popup.connect(&v6_surface->events.new_popup);

    on_set_title.connect(&v6_surface->toplevel->events.set_title);
    on_set_app_id.connect(&v6_surface->toplevel->events.set_app_id);
    on_set_parent.connect(&v6_surface->toplevel->events.set_parent);
    on_request_move.connect(&v6_surface->toplevel->events.request_move);
    on_request_resize.connect(&v6_surface->toplevel->events.request_resize);
    on_request_maximize.connect(&v6_surface->toplevel->events.request_maximize);
    on_request_minimize.connect(&v6_surface->toplevel->events.request_minimize);
    on_request_fullscreen.connect(&v6_surface->toplevel->events.request_fullscreen);

    wlr_xdg_surface_v6_ping(s);
    v6_surface->data = this;
}

static wf_geometry get_xdg_geometry(wlr_xdg_surface_v6 *surface)
{
    wlr_box xdg_geometry;
    wlr_xdg_surface_v6_get_geometry(surface, &xdg_geometry);
    return xdg_geometry;
}

void wayfire_xdg6_view::commit()
{
    wayfire_view_t::commit();

    auto v6_geometry = get_xdg_geometry(v6_surface);
    if (v6_geometry.x != xdg_surface_offset.x ||
        v6_geometry.y != xdg_surface_offset.y)
    {
        auto wm = get_wm_geometry();
        xdg_surface_offset = {v6_geometry.x, v6_geometry.y};
        move(wm.x, wm.y, false);
    }
}

void wayfire_xdg6_view::map(wlr_surface *surface)
{
    auto v6_geometry = get_xdg_geometry(v6_surface);
    xdg_surface_offset = {v6_geometry.x, v6_geometry.y};

    if (v6_surface->toplevel->client_pending.maximized)
        maximize_request(true);

    if (v6_surface->toplevel->client_pending.fullscreen)
        fullscreen_request(output, true);

    if (v6_surface->toplevel->parent)
    {
        auto parent = wf_view_from_void(v6_surface->toplevel->parent->data)->self();
        set_toplevel_parent(parent);
    }

    wayfire_view_t::map(surface);
    create_toplevel();
}

void wayfire_xdg6_view::get_child_offset(int &x, int &y)
{
    x = xdg_surface_offset.x;
    y = xdg_surface_offset.y;
}

wf_geometry wayfire_xdg6_view::get_wm_geometry()
{
    if (!v6_surface)
        return get_untransformed_bounding_box();

    auto opos = get_output_position();
    auto xdg_geometry = get_xdg_geometry(v6_surface);
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

void wayfire_xdg6_view::activate(bool act)
{
    wlr_xdg_toplevel_v6_set_activated(v6_surface, act);
    wayfire_view_t::activate(act);
}

void wayfire_xdg6_view::set_maximized(bool max)
{
    wayfire_view_t::set_maximized(max);
    wlr_xdg_toplevel_v6_set_maximized(v6_surface, max);
}

void wayfire_xdg6_view::set_fullscreen(bool full)
{
    wayfire_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_v6_set_fullscreen(v6_surface, full);
}

void wayfire_xdg6_view::move(int w, int h, bool send)
{
    wayfire_view_t::move(w, h, send);
}

void wayfire_xdg6_view::resize(int w, int h, bool send)
{
    damage();
    if (frame)
        frame->calculate_resize_size(w, h);

    wlr_xdg_toplevel_v6_set_size(v6_surface, w, h);
}

void wayfire_xdg6_view::request_native_size()
{
    wlr_xdg_toplevel_v6_set_size(v6_surface, 0, 0);
}

std::string wayfire_xdg6_view::get_app_id()
{
    return nonull(v6_surface->toplevel->app_id);
}

std::string wayfire_xdg6_view::get_title()
{
    return nonull(v6_surface->toplevel->title);
}

void wayfire_xdg6_view::close()
{
    wlr_xdg_surface_v6_send_close(v6_surface);
    wayfire_view_t::close();
}

wayfire_xdg6_view::~wayfire_xdg6_view()
{ }

void wayfire_xdg6_view::destroy()
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

    v6_surface = nullptr;
    wayfire_view_t::destroy();
}

static wlr_xdg_shell_v6 *v6_handle;
void init_xdg_shell_v6()
{
    static wf::wl_listener_wrapper on_created;
    v6_handle = wlr_xdg_shell_v6_create(core->display);
    if (v6_handle)
    {
        on_created.set_callback([=] (void *data) {
            auto surf = static_cast<wlr_xdg_surface_v6*> (data);
            if (surf->role == WLR_XDG_SURFACE_V6_ROLE_TOPLEVEL)
                core->add_view(std::make_unique<wayfire_xdg6_view> (surf));
        });
        on_created.connect(&v6_handle->events.new_surface);
    }
}
