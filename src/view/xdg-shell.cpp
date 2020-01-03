#include <wayfire/util/log.hpp>
#include <wayfire/debug.hpp>
#include "wayfire/core.hpp"
#include "surface-impl.hpp"
#include "wayfire/output.hpp"
#include "wayfire/decorator.hpp"
#include "xdg-shell.hpp"
#include "wayfire/output-layout.hpp"
#include <wayfire/workspace-manager.hpp>

template<class XdgPopupVersion>
wayfire_xdg_popup<XdgPopupVersion>::wayfire_xdg_popup(XdgPopupVersion *popup)
    : wf::wlr_view_t()
{
    this->popup_parent =
        dynamic_cast<wlr_view_t*>(wf::wf_surface_from_void(popup->parent->data));
    this->popup = popup;
    this->role = wf::VIEW_ROLE_UNMANAGED;
    this->view_impl->keyboard_focus_enabled = false;
    this->set_output(popup_parent->get_output());
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::initialize()
{
    LOGI("New xdg popup");
    on_map.set_callback([&] (void*) { map(this->popup->base->surface); });
    on_unmap.set_callback([&] (void*) {
        pending_close.disconnect();
        unmap();
    });
    on_destroy.set_callback([&] (void*) { destroy(); });
    on_new_popup.set_callback([&] (void* data) {
        create_xdg_popup((XdgPopupVersion*) data);
    });

    on_map.connect(&popup->base->events.map);
    on_unmap.connect(&popup->base->events.unmap);
    on_destroy.connect(&popup->base->events.destroy);
    on_new_popup.connect(&popup->base->events.new_popup);

    popup->base->data = this;
    parent_geometry_changed.set_callback([=] (wf::signal_data_t*) {
        this->update_position();
    });
    parent_app_id_changed.set_callback([=] (wf::signal_data_t*) {
        this->handle_app_id_changed(popup_parent->get_app_id());
    });
    parent_title_changed.set_callback([=] (wf::signal_data_t*) {
        this->handle_title_changed(popup_parent->get_title());
    });

    popup_parent->connect_signal("geometry-changed",
        &this->parent_geometry_changed);
    popup_parent->connect_signal("app-id-changed",
        &this->parent_app_id_changed);
    popup_parent->connect_signal("title-changed",
        &this->parent_title_changed);
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::map(wlr_surface *surface)
{
    uint32_t parent_layer =
        get_output()->workspace->get_view_layer(popup_parent->self());

    wf::layer_t target_layer = wf::LAYER_XWAYLAND;
    if (parent_layer > wf::LAYER_WORKSPACE)
        target_layer = (wf::layer_t)parent_layer;
    get_output()->workspace->add_view(self(), target_layer);

    wlr_view_t::map(surface);
    update_position();
    unconstrain();
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::commit()
{
    wlr_view_t::commit();
    update_position();
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::update_position()
{
    if (!popup_parent->is_mapped() || !is_mapped())
        return;

    wf::pointf_t popup_offset = {
        1.0 * popup->geometry.x + popup_parent->get_window_offset().x,
        1.0 * popup->geometry.y + popup_parent->get_window_offset().y,
    };

    auto parent_geometry = popup_parent->get_output_geometry();
    popup_offset.x += parent_geometry.x - get_window_offset().x;
    popup_offset.y += parent_geometry.y - get_window_offset().y;

    popup_offset = popup_parent->transform_point(popup_offset);
    this->move(popup_offset.x, popup_offset.y);
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::unconstrain()
{
    wf::view_interface_t *toplevel_parent = this;
    using popup_type = wayfire_xdg_popup<XdgPopupVersion>*;
    while (true)
    {
        popup_type as_popup = dynamic_cast<popup_type> (toplevel_parent);
        if (as_popup) {
            toplevel_parent = as_popup->popup_parent;
        } else {
            break;
        }
    }

    if (!get_output() || !toplevel_parent)
        return;

    auto box = get_output()->get_relative_geometry();
    auto wm = toplevel_parent->get_output_geometry();
    box.x -= wm.x;
    box.y -= wm.y;

    _do_unconstrain(box);
}

template<>
void wayfire_xdg_popup<wlr_xdg_popup>::_do_unconstrain(wlr_box box) {
    wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

template<>
void wayfire_xdg_popup<wlr_xdg_popup_v6>::_do_unconstrain(wlr_box box) {
    wlr_xdg_popup_v6_unconstrain_from_box(popup, &box);
}

template<class XdgPopupVersion>
void wayfire_xdg_popup<XdgPopupVersion>::destroy()
{
    on_map.disconnect();
    on_unmap.disconnect();
    on_destroy.disconnect();
    on_new_popup.disconnect();

    wlr_view_t::destroy();
}

template<class XdgPopupVersion>
wf::point_t wayfire_xdg_popup<XdgPopupVersion>::get_window_offset()
{
    return {
        popup->base->geometry.x,
        popup->base->geometry.y,
    };
}

template<>
void wayfire_xdg_popup<wlr_xdg_popup_v6>::close()
{
    pending_close.run_once([=] () {
        if (is_mapped())
            wlr_xdg_surface_v6_send_close(popup->base);
    });
}

template<>
void wayfire_xdg_popup<wlr_xdg_popup>::close()
{
    pending_close.run_once([=] () {
        if (is_mapped())
            wlr_xdg_popup_destroy(popup->base);
    });
}

template<class XdgPopupVersion>
void create_xdg_popup_templ(XdgPopupVersion *popup)
{
    auto parent = wf::wf_surface_from_void(popup->parent->data);
    if (!parent)
    {
        LOGE("attempting to create a popup with unknown parent");
        return;
    }

    wf::get_core().add_view(
        std::make_unique<wayfire_xdg_popup<XdgPopupVersion>> (popup));
}

template<class XdgPopupVersion>
void create_xdg_popup(XdgPopupVersion *popup) { create_xdg_popup_templ(popup); }

// specialized in header
template<> void create_xdg_popup<wlr_xdg_popup> (wlr_xdg_popup *popup) {
    create_xdg_popup_templ(popup);
}

template<class XdgToplevelVersion>
wayfire_xdg_view<XdgToplevelVersion>::wayfire_xdg_view(XdgToplevelVersion *top)
    : wf::wlr_view_t(), xdg_toplevel(top)
{ }

template<class XdgToplevelVersion>
void wayfire_xdg_view<XdgToplevelVersion>::initialize()
{
    wlr_view_t::initialize();
    LOGI("new xdg_shell_stable surface: ", xdg_toplevel->title,
        " app-id: ", xdg_toplevel->app_id);

    handle_title_changed(nonull(xdg_toplevel->title));
    handle_app_id_changed(nonull(xdg_toplevel->app_id));

    on_map.set_callback([&] (void*) { map(xdg_toplevel->base->surface); });
    on_unmap.set_callback([&] (void*) { unmap(); });
    on_destroy.set_callback([&] (void*) { destroy(); });
    on_new_popup.set_callback([&] (void* data) {
        create_xdg_popup((decltype(xdg_toplevel->base->popup))data);
    });

    on_set_title.set_callback([&] (void*) {
        handle_title_changed(nonull(xdg_toplevel->title));
    });
    on_set_app_id.set_callback([&] (void*) {
        handle_app_id_changed(nonull(xdg_toplevel->app_id));
    });
    on_set_parent.set_callback([&] (void*) {
        auto parent = xdg_toplevel->parent ?
            wf::wf_view_from_void(xdg_toplevel->parent->data)->self() : nullptr;
        set_toplevel_parent(parent);
    });

    on_request_move.set_callback([&] (void*) { move_request(); });
    on_request_resize.set_callback([&] (void*) { resize_request(); });
    on_request_minimize.set_callback([&] (void*) { minimize_request(true); });
    on_request_maximize.set_callback([&] (void* data) {
        tile_request(xdg_toplevel->client_pending.maximized ?
            wf::TILED_EDGES_ALL : 0);
    });
    on_request_fullscreen.set_callback([&] (void* data) {
        auto ev = static_cast<wlr_xdg_toplevel_set_fullscreen_event*> (data);
        auto wo = wf::get_core().output_layout->find_output(ev->output);
        fullscreen_request(wo, ev->fullscreen);
    });

    on_map.connect(&xdg_toplevel->base->events.map);
    on_unmap.connect(&xdg_toplevel->base->events.unmap);
    on_destroy.connect(&xdg_toplevel->base->events.destroy);
    on_new_popup.connect(&xdg_toplevel->base->events.new_popup);

    on_set_title.connect(&xdg_toplevel->events.set_title);
    on_set_app_id.connect(&xdg_toplevel->events.set_app_id);
    on_set_parent.connect(&xdg_toplevel->events.set_parent);
    on_request_move.connect(&xdg_toplevel->events.request_move);
    on_request_resize.connect(&xdg_toplevel->events.request_resize);
    on_request_maximize.connect(&xdg_toplevel->events.request_maximize);
    on_request_minimize.connect(&xdg_toplevel->events.request_minimize);
    on_request_fullscreen.connect(&xdg_toplevel->events.request_fullscreen);

    xdg_toplevel->base->data = dynamic_cast<view_interface_t*> (this);
    // set initial parent
    on_set_parent.emit(nullptr);

    if (xdg_toplevel->client_pending.fullscreen)
        fullscreen_request(get_output(), true);

    if (xdg_toplevel->client_pending.maximized)
        tile_request(wf::TILED_EDGES_ALL);
}

template<class XdgToplevelVersion>
wayfire_xdg_view<XdgToplevelVersion>::~wayfire_xdg_view() {}

template<class XdgToplevelVersion>
wf::geometry_t get_xdg_geometry(XdgToplevelVersion *toplevel);

template<>
wf::geometry_t get_xdg_geometry<wlr_xdg_toplevel>(wlr_xdg_toplevel *toplevel)
{
    wlr_box xdg_geometry;
    wlr_xdg_surface_get_geometry(toplevel->base, &xdg_geometry);
    return xdg_geometry;
}

template<>
wf::geometry_t get_xdg_geometry<wlr_xdg_toplevel_v6>(wlr_xdg_toplevel_v6 *toplevel)
{
    wlr_box xdg_geometry;
    wlr_xdg_surface_v6_get_geometry(toplevel->base, &xdg_geometry);
    return xdg_geometry;
}

template<class XdgToplevelVersion>
void wayfire_xdg_view<XdgToplevelVersion>::map(wlr_surface *surface)
{
    wlr_view_t::map(surface);
    create_toplevel();
}

template<class XdgToplevelVersion>
void wayfire_xdg_view<XdgToplevelVersion>::commit()
{
    wlr_view_t::commit();

    /* On each commit, check whether the window geometry of the xdg_surface
     * changed. In those cases, we need to adjust the view's output geometry,
     * so that the apparent wm geometry doesn't change */
    auto wm = get_wm_geometry();
    auto xdg_g = get_xdg_geometry(xdg_toplevel);
    if (xdg_g.x != xdg_surface_offset.x || xdg_g.y != xdg_surface_offset.y)
    {
        xdg_surface_offset = {xdg_g.x, xdg_g.y};
        /* Note that we just changed the xdg_surface offset, which means we
         * also changed the wm geometry. Plugins which depend on the
         * geometry-changed signal however need to receive the appropriate
         * old geometry */
        set_position(wm.x, wm.y, wm, true);
    }
}

template<class XdgToplevelVersion>
wf::point_t wayfire_xdg_view<XdgToplevelVersion>::get_window_offset()
{
    return xdg_surface_offset;
}

template<class XdgToplevelVersion>
wf::geometry_t wayfire_xdg_view<XdgToplevelVersion>::get_wm_geometry()
{
    if (!is_mapped())
        return get_output_geometry();

    auto output_g = get_output_geometry();
    auto xdg_geometry = get_xdg_geometry(xdg_toplevel);

    wf::geometry_t wm = {
        output_g.x + xdg_surface_offset.x,
        output_g.y + xdg_surface_offset.y,
        xdg_geometry.width,
        xdg_geometry.height
    };

    if (view_impl->frame)
        wm = view_impl->frame->expand_wm_geometry(wm);

    return wm;
}

template<class XdgToplevelVersion>
void wayfire_xdg_view<XdgToplevelVersion>::set_activated(bool act)
{
    /* we don't send activated or deactivated for shell views,
     * they should always be active */
    if (this->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT)
        act = true;

    _set_activated(act);
    wf::wlr_view_t::set_activated(act);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::_set_activated(bool act) {
    wlr_xdg_toplevel_set_activated(xdg_toplevel->base, act);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::_set_activated(bool act) {
    wlr_xdg_toplevel_v6_set_activated(xdg_toplevel->base, act);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::set_tiled(uint32_t edges)
{
    wlr_xdg_toplevel_set_tiled(xdg_toplevel->base, edges);
    wlr_xdg_toplevel_set_maximized(xdg_toplevel->base,
        (edges == wf::TILED_EDGES_ALL));
    wlr_view_t::set_tiled(edges);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::set_tiled(uint32_t edges) {
    wlr_xdg_toplevel_v6_set_maximized(xdg_toplevel->base, !!edges);
    wlr_view_t::set_tiled(edges);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::set_fullscreen(bool full)
{
    wf::wlr_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_set_fullscreen(xdg_toplevel->base, full);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::set_fullscreen(bool full)
{
    wf::wlr_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_v6_set_fullscreen(xdg_toplevel->base, full);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::resize(int w, int h)
{
    if (view_impl->frame)
        view_impl->frame->calculate_resize_size(w, h);
    wlr_xdg_toplevel_set_size(xdg_toplevel->base, w, h);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::resize(int w, int h)
{
    if (view_impl->frame)
        view_impl->frame->calculate_resize_size(w, h);
    wlr_xdg_toplevel_v6_set_size(xdg_toplevel->base, w, h);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::request_native_size() {
    wlr_xdg_toplevel_set_size(xdg_toplevel->base, 0, 0);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::request_native_size() {
    wlr_xdg_toplevel_v6_set_size(xdg_toplevel->base, 0, 0);
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel>::close() {
    wlr_xdg_toplevel_send_close(xdg_toplevel->base);
    wf::wlr_view_t::close();
}

template<>
void wayfire_xdg_view<wlr_xdg_toplevel_v6>::close() {
    wlr_xdg_surface_v6_send_close(xdg_toplevel->base);
    wf::wlr_view_t::close();
}

template<class XdgToplevelVersion>
void wayfire_xdg_view<XdgToplevelVersion>::destroy()
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

    xdg_toplevel = nullptr;
    wf::wlr_view_t::destroy();
}

static wlr_xdg_shell *xdg_handle;
static wlr_xdg_shell_v6 *xdg_handle_v6;


void wf::init_xdg_shell()
{
    static wf::wl_listener_wrapper on_xdg_created, on_xdg6_created;
    xdg_handle = wlr_xdg_shell_create(wf::get_core().display);

    if (xdg_handle)
    {
        on_xdg_created.set_callback([&] (void *data) {
            auto surf = static_cast<wlr_xdg_surface*> (data);
            if (surf->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
            {
                wf::get_core().add_view(
                    std::make_unique<wayfire_xdg_view<wlr_xdg_toplevel>> (
                        surf->toplevel));
            }
        });
        on_xdg_created.connect(&xdg_handle->events.new_surface);
    }

    xdg_handle_v6 = wlr_xdg_shell_v6_create(wf::get_core().display);
    if (xdg_handle_v6)
    {
        on_xdg6_created.set_callback([&] (void *data) {
            auto surf = static_cast<wlr_xdg_surface_v6*> (data);
            if (surf->role == WLR_XDG_SURFACE_V6_ROLE_TOPLEVEL)
            {
                wf::get_core().add_view(
                    std::make_unique<wayfire_xdg_view<wlr_xdg_toplevel_v6>> (
                        surf->toplevel));
            }
        });
        on_xdg6_created.connect(&xdg_handle_v6->events.new_surface);
    }
}

template class wayfire_xdg_popup<wlr_xdg_popup_v6>;
template class wayfire_xdg_popup<wlr_xdg_popup>;
template class wayfire_xdg_view<wlr_xdg_toplevel_v6>;
template class wayfire_xdg_view<wlr_xdg_toplevel>;
