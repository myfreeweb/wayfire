#include "priv-view.hpp"
#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "workspace-manager.hpp"

extern "C"
{
#define class class_t
#define static
#include <wlr/xwayland.h>
#undef static
#undef class
}

class wayfire_unmanaged_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;
    wl_listener destroy_ev, unmap_listener, map_ev, configure;

    public:
    wayfire_unmanaged_xwayland_view(wlr_xwayland_surface *xww);
    bool is_subsurface() { return false; }

    int global_x, global_y;

    void commit();
    void map(wlr_surface *surface);
    void unmap();
    void activate(bool active);
    void send_configure();
    void move(int x, int y, bool s);
    void resize(int w, int h, bool s);
    void set_geometry(wf_geometry g);
    void set_output(wayfire_output *wo);
    void close();
    wlr_surface *get_keyboard_focus_surface();
    virtual bool should_be_decorated() { return false; }
    ~wayfire_unmanaged_xwayland_view() { }
    virtual void destroy();
    std::string get_title()  { return nonull(xw->title);   }
    std::string get_app_id() { return nonull(xw->class_t); }
};

static void handle_xwayland_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_move_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->move_request();
}

static void handle_xwayland_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_resize_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->resize_request(ev->edges);
}

static void handle_xwayland_request_configure(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_surface_configure_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);

    view->set_geometry({ev->x, ev->y, ev->width, ev->height});
}

static void handle_xwayland_request_maximize(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->maximize_request(surf->maximized_horz && surf->maximized_vert);
}

static void handle_xwayland_request_fullscreen(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->fullscreen_request(view->get_output(), surf->fullscreen);
}

static void handle_xwayland_map(wl_listener* listener, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland map %p %p -> %p", xsurf, xsurf->surface, view);
    view->map(xsurf->surface);
}

static void handle_xwayland_unmap(wl_listener*, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland unmap %p", xsurf);
    view->unmap();
}

static void handle_xwayland_destroy(wl_listener*, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland destroy %p", xsurf);
    view->destroy();
}

static void handle_xwayland_set_parent(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    auto parent = surface->parent ?
        wf_view_from_void(surface->parent->data)->self() : nullptr;

    assert(view);
    view->set_toplevel_parent(parent);
}

static void handle_xwayland_set_title(wl_listener *listener, void *data)
{
    auto surface = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    view->handle_title_changed();
}

static void handle_xwayland_set_app_id(wl_listener *listener, void *data)
{
    auto surface = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    view->handle_app_id_changed();
}

class wayfire_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;

    /* TODO: very bad names, also in other shells */
    wl_listener destroy_ev, map_ev, unmap_ev, configure,
                request_move, request_resize,
                request_maximize, request_fullscreen,
                set_parent_ev, set_title, set_app_id;

    signal_callback_t output_geometry_changed;

    public:
    wayfire_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        log_info("new xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        destroy_ev.notify         = handle_xwayland_destroy;
        map_ev.notify             = handle_xwayland_map;
        unmap_ev.notify           = handle_xwayland_unmap;
        configure.notify          = handle_xwayland_request_configure;
        set_title.notify          = handle_xwayland_set_title;
        set_app_id.notify         = handle_xwayland_set_app_id;
        set_parent_ev.notify      = handle_xwayland_set_parent;
        request_move.notify       = handle_xwayland_request_move;
        request_resize.notify     = handle_xwayland_request_resize;
        request_maximize.notify   = handle_xwayland_request_maximize;
        request_fullscreen.notify = handle_xwayland_request_fullscreen;

        wl_signal_add(&xw->events.destroy,            &destroy_ev);
        wl_signal_add(&xw->events.unmap,              &unmap_ev);
        wl_signal_add(&xw->events.map,                &map_ev);
        wl_signal_add(&xw->events.set_title,          &set_title);
        wl_signal_add(&xw->events.set_class,          &set_app_id);
        wl_signal_add(&xw->events.set_parent,         &set_parent_ev);
        wl_signal_add(&xw->events.request_move,       &request_move);
        wl_signal_add(&xw->events.request_resize,     &request_resize);
        wl_signal_add(&xw->events.request_maximize,   &request_maximize);
        wl_signal_add(&xw->events.request_fullscreen, &request_fullscreen);
        wl_signal_add(&xw->events.request_configure,  &configure);

        xw->data = this;

        output_geometry_changed = [this] (signal_data*)
        {
            if (is_mapped())
                move(geometry.x, geometry.y, false);
        };
    }

    ~wayfire_xwayland_view()
    {}

    virtual void destroy()
    {
        if (output)
            output->disconnect_signal("output-resized", &output_geometry_changed);

        wl_list_remove(&destroy_ev.link);
        wl_list_remove(&unmap_ev.link);
        wl_list_remove(&map_ev.link);
        wl_list_remove(&set_parent_ev.link);
        wl_list_remove(&request_move.link);
        wl_list_remove(&request_resize.link);
        wl_list_remove(&request_maximize.link);
        wl_list_remove(&request_fullscreen.link);
        wl_list_remove(&configure.link);
        wl_list_remove(&set_title.link);
        wl_list_remove(&set_app_id.link);

        wayfire_view_t::destroy();
    }

    void map(wlr_surface *surface)
    {
        /* override-redirect status changed between creation and MapNotify */
        if (xw->override_redirect)
        {
            auto xsurface = xw; // keep the xsurface in stack, because destroy will likely free this
            destroy();

            auto view = nonstd::make_unique<wayfire_unmanaged_xwayland_view> (xsurface);
            auto raw = view.get();

            core->add_view(std::move(view));
            raw->map(xsurface->surface);
            return;
        }

        if (xw->maximized_horz && xw->maximized_vert)
            maximize_request(true);

        if (xw->fullscreen)
            fullscreen_request(output, true);

        if (xw->parent)
        {
            auto parent = wf_view_from_void(xw->parent->data)->self();
            set_toplevel_parent(parent);
        }

        auto real_output = output->get_full_geometry();
        if (!maximized && !fullscreen && !parent)
            move(xw->x - real_output.x, xw->y - real_output.y, false);

        wayfire_view_t::map(surface);
        create_toplevel();
    }

    bool is_subsurface() { return false; }

    virtual bool should_be_decorated()
    {
        return !(xw->decorations & (WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE |
                                    WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER));
    }

    void activate(bool active)
    {
        wlr_xwayland_surface_activate(xw, active);
        wayfire_view_t::activate(active);
    }

    void send_configure(int width, int height)
    {
        if (width < 0 || height < 0)
        {
            /* such a configure request would freeze xwayland. This is most probably a bug */
            log_error("Configuring a xwayland surface with width/height <0");
            return;
        }

        auto output_geometry = get_output_geometry();

        int configure_x = output_geometry.x;
        int configure_y = output_geometry.y;

        if (output)
        {
            auto real_output = output->get_full_geometry();
            configure_x += real_output.x;
            configure_y += real_output.y;
        }

        wlr_xwayland_surface_configure(xw, configure_x, configure_y, width, height);
    }

    void send_configure()
    {
        send_configure(geometry.width, geometry.height);
    }

    void set_output(wayfire_output *wo)
    {
        if (output)
            output->disconnect_signal("output-resized", &output_geometry_changed);

        wayfire_view_t::set_output(wo);

        if (wo)
            wo->connect_signal("output-resized", &output_geometry_changed);

        send_configure();
    }

    void move(int x, int y, bool s)
    {
        wayfire_view_t::move(x, y, s);
        if (!destroyed && !in_continuous_move)
            send_configure();
    }

    void set_moving(bool moving)
    {
        wayfire_view_t::set_moving(moving);

        /* We don't send updates while in continuous move, because that means
         * too much configure requests. Instead, we set it at the end */
        if (!in_continuous_move)
            send_configure();
    }

    void resize(int w, int h, bool s)
    {
        damage();
        if (frame)
            frame->calculate_resize_size(w, h);
        send_configure(w, h);
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();

        wayfire_view_t::move(g.x, g.y, false);
        resize(g.width, g.height, false);
    }

    void close()
    {
        wlr_xwayland_surface_close(xw);
        wayfire_view_t::close();
    }

    void set_maximized(bool maxim)
    {
        wayfire_view_t::set_maximized(maxim);
        wlr_xwayland_surface_set_maximized(xw, maxim);
    }

    std::string get_title() { return nonull(xw->title); }
    std::string get_app_id() {return nonull(xw->class_t); }

    virtual void toplevel_send_app_id()
    {
        if (!toplevel_handle)
            return;

        std::string app_id;

        auto default_app_id = get_app_id();
        auto instance_app_id = nonull(xw->instance);

        auto app_id_mode = (*core->config)["workarounds"]
            ->get_option("app_id_mode", "stock");

        if (app_id_mode->as_string() == "full") {
            app_id = default_app_id + " " + instance_app_id;
        } else {
            app_id = default_app_id;
        }

        wlr_foreign_toplevel_handle_v1_set_app_id(toplevel_handle, app_id.c_str());
    }

    void set_fullscreen(bool full)
    {
        wayfire_view_t::set_fullscreen(full);
        wlr_xwayland_surface_set_fullscreen(xw, full);
    }
};

wayfire_unmanaged_xwayland_view::wayfire_unmanaged_xwayland_view(wlr_xwayland_surface *xww)
    : wayfire_view_t(), xw(xww)
{
    log_info("new unmanaged xwayland surface %s class: %s instance: %s",
             nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

    map_ev.notify         = handle_xwayland_map;
    destroy_ev.notify     = handle_xwayland_destroy;
    unmap_listener.notify = handle_xwayland_unmap;
    configure.notify      = handle_xwayland_request_configure;

    wl_signal_add(&xw->events.destroy,            &destroy_ev);
    wl_signal_add(&xw->events.unmap,              &unmap_listener);
    wl_signal_add(&xw->events.request_configure,  &configure);
    wl_signal_add(&xw->events.map,                &map_ev);

    xw->data = this;
    role = WF_VIEW_ROLE_UNMANAGED;
}

void wayfire_unmanaged_xwayland_view::commit()
{
    if (global_x != xw->x || global_y != xw->y)
    {
        global_x = xw->x;
        global_y = xw->y;

        if (output)
        {
            auto real_output = output->get_full_geometry();
            wayfire_view_t::move(xw->x - real_output.x, xw->y - real_output.y, false);
        }
    }

    wayfire_surface_t::commit();

    auto old_geometry = geometry;
    if (update_size())
    {
        damage(old_geometry);
        damage();
    }
}

void wayfire_unmanaged_xwayland_view::map(wlr_surface *surface)
{
    _is_mapped = true;

    /* move to the output where our center is
     * FIXME: this is a bad idea, because a dropdown menu might get sent to
     * an incorrect output. However, no matter how we calculate the real
     * output, we just can't be 100% compatible because in X all windows are
     * positioned in a global coordinate space */
    auto wo = core->get_output_at(xw->x + surface->current.width / 2, xw->y + surface->current.height / 2);

    if (!wo)
    {
        /* if surface center is outside of anything, try to check the output where the pointer is */
        GetTuple(cx, cy, core->get_cursor_position());
        wo = core->get_output_at(cx, cy);
    }

    if (!wo)
        wo = core->get_active_output();
    assert(wo);

    if (wo != output)
    {
        if (output)
            output->workspace->add_view_to_layer(self(), 0);
        set_output(wo);
    }

    auto real_output = output->get_full_geometry();

    wayfire_view_t::move(xw->x - real_output.x, xw->y - real_output.y, false);
    global_x = xw->x;
    global_y = xw->y;

    damage();

    wayfire_surface_t::map(surface);
    output->workspace->add_view_to_layer(self(), WF_LAYER_XWAYLAND);

    if (wlr_xwayland_or_surface_wants_focus(xw))
    {
        auto wa = output->workspace->get_workarea();
        move(xw->x + wa.x - real_output.x, xw->y + wa.y - real_output.y, false);

        emit_view_map(self());
        output->focus_view(self());
    }
}

void wayfire_unmanaged_xwayland_view::unmap()
{
    _is_mapped = false;

    if (wlr_xwayland_or_surface_wants_focus(xw))
        emit_view_unmap(self());

    wayfire_surface_t::unmap();
}

void wayfire_unmanaged_xwayland_view::activate(bool active)
{
    wayfire_view_t::activate(active);
    wlr_xwayland_surface_activate(xw, active);
}

void wayfire_unmanaged_xwayland_view::send_configure()
{
    if (geometry.width < 0 || geometry.height < 0)
    {
        /* such a configure request would freeze xwayland. This is most probably a bug */
        log_error("Configuring a xwayland surface with width/height <0");
        return;
    }


    if (output)
    {
        auto real_output = output->get_full_geometry();
        global_x = geometry.x + real_output.x;
        global_y = geometry.y + real_output.y;
    } else
    {
        global_x = geometry.x;
        global_y = geometry.y;
    }

    wlr_xwayland_surface_configure(xw, global_x, global_y, geometry.width, geometry.height);
    damage();
}

void wayfire_unmanaged_xwayland_view::move(int x, int y, bool s)
{
    damage();
    geometry.x = x;
    geometry.y = y;
    send_configure();
}

void wayfire_unmanaged_xwayland_view::resize(int w, int h, bool s)
{
    damage();
    geometry.width = w;
    geometry.height = h;
    send_configure();
}

void wayfire_unmanaged_xwayland_view::set_geometry(wf_geometry g)
{
    damage();
    geometry = g;
    send_configure();
}

void wayfire_unmanaged_xwayland_view::set_output(wayfire_output *wo)
{
    wayfire_surface_t::set_output(wo);

    if (wo)
    {
        auto real_geometry = wo->get_full_geometry();
        wayfire_view_t::move(global_x - real_geometry.x, global_y - real_geometry.y);
    }
}

void wayfire_unmanaged_xwayland_view::close()
{
    wlr_xwayland_surface_close(xw);
}

wlr_surface *wayfire_unmanaged_xwayland_view::get_keyboard_focus_surface()
{
    if (!wlr_xwayland_or_surface_wants_focus(xw))
        return nullptr;
    return wayfire_view_t::get_keyboard_focus_surface();
}

void wayfire_unmanaged_xwayland_view::destroy()
{
    wl_list_remove(&destroy_ev.link);
    wl_list_remove(&unmap_listener.link);
    wl_list_remove(&configure.link);
    wl_list_remove(&map_ev.link);

    wayfire_view_t::destroy();
}

void notify_xwayland_created(wl_listener *, void *data)
{
    auto xsurf = (wlr_xwayland_surface*) data;

    if (xsurf->override_redirect)
    {
        core->add_view(nonstd::make_unique<wayfire_unmanaged_xwayland_view>(xsurf));
    } else
    {
        core->add_view(nonstd::make_unique<wayfire_xwayland_view> (xsurf));
    }
}

static wlr_xwayland *xwayland_handle;
static wl_listener xwayland_created;

void init_xwayland()
{
    xwayland_created.notify = notify_xwayland_created;
    xwayland_handle = wlr_xwayland_create(core->display, core->compositor, false);

    if (xwayland_handle)
        wl_signal_add(&xwayland_handle->events.new_surface, &xwayland_created);
}

void xwayland_set_seat(wlr_seat *seat)
{
    if (xwayland_handle)
        wlr_xwayland_set_seat(xwayland_handle, core->get_current_seat());
}

std::string xwayland_get_display()
{
    return std::to_string(xwayland_handle ? xwayland_handle->display : -1);
}
