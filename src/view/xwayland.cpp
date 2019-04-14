#include "priv-view.hpp"
#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "workspace-manager.hpp"
#include "output-layout.hpp"

extern "C"
{
#include <wlr/config.h>

#if WLR_HAS_XWAYLAND
#define class class_t
#define static
#include <wlr/xwayland.h>
#undef static
#undef class
#endif
}

#if WLR_HAS_XWAYLAND

class wayfire_xwayland_view_base : public wayfire_view_t
{
    protected:
    wf::wl_listener_wrapper on_destroy, on_unmap, on_map, on_configure;

    wlr_xwayland_surface *xw;
    int last_server_width = 0;
    int last_server_height = 0;

    signal_callback_t output_geometry_changed = [this] (signal_data*)
    {
        if (is_mapped())
            move(geometry.x, geometry.y, false);
    };

    public:

    wayfire_xwayland_view_base(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        on_map.set_callback([&] (void*) { map(xw->surface); });
        on_unmap.set_callback([&] (void*) { unmap(); });
        on_destroy.set_callback([&] (void*) { destroy(); });
        on_configure.set_callback([&] (void* data) {
            auto ev = static_cast<wlr_xwayland_surface_configure_event*> (data);
            configure_request({ev->x, ev->y, ev->width, ev->height});
        });

        on_map.connect(&xw->events.map);
        on_unmap.connect(&xw->events.unmap);
        on_destroy.connect(&xw->events.destroy);
        on_configure.connect(&xw->events.request_configure);
    }

    virtual void destroy() override
    {
        if (output)
            output->disconnect_signal("output-configuration-changed", &output_geometry_changed);

        on_map.disconnect();
        on_unmap.disconnect();
        on_destroy.disconnect();
        on_configure.disconnect();

        wayfire_view_t::destroy();
    }

    virtual void configure_request(wf_geometry configure_geometry)
    {
        /* Wayfire positions views relative to their output, but Xwayland windows
         * have a global positioning. So, we need to make sure that we always
         * transform between output-local coordinates and global coordinates */
        if (output)
        {
            auto og = output->get_layout_geometry();
            configure_geometry.x -= og.x;
            configure_geometry.y -= og.y;
        }

        if (frame)
            configure_geometry = frame->expand_wm_geometry(configure_geometry);
        set_geometry(configure_geometry);
    }

    virtual bool is_subsurface() override { return false; }
    virtual std::string get_title()  override { return nonull(xw->title);   }
    virtual std::string get_app_id() override { return nonull(xw->class_t); }

    virtual void close() override
    {
        wlr_xwayland_surface_close(xw);
        wayfire_view_t::close();
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
            auto real_output = output->get_layout_geometry();
            configure_x += real_output.x;
            configure_y += real_output.y;
        }

        if (_is_mapped)
        {
            wlr_xwayland_surface_configure(xw,
                configure_x, configure_y, width, height);
        }
    }

    void send_configure()
    {
        send_configure(last_server_width, last_server_height);
    }

    virtual void set_output(wayfire_output *wo) override
    {
        if (output)
            output->disconnect_signal("output-configuration-changed", &output_geometry_changed);

        wayfire_view_t::set_output(wo);

        if (wo)
            wo->connect_signal("output-configuration-changed", &output_geometry_changed);

        send_configure();
    }
};

class wayfire_unmanaged_xwayland_view : public wayfire_xwayland_view_base
{
    public:
    wayfire_unmanaged_xwayland_view(wlr_xwayland_surface *xww);

    int global_x, global_y;

    void commit();
    void map(wlr_surface *surface);
    void unmap();
    void activate(bool active);
    void move(int x, int y, bool s);
    void resize(int w, int h, bool s);
    void set_geometry(wf_geometry g);
    wlr_surface *get_keyboard_focus_surface();

    virtual bool should_be_decorated() { return false; }
    ~wayfire_unmanaged_xwayland_view() { }
};

class wayfire_xwayland_view : public wayfire_xwayland_view_base
{
    wf::wl_listener_wrapper on_request_move, on_request_resize,
        on_request_maximize, on_request_fullscreen,
        on_set_parent, on_set_title, on_set_app_id;

    public:
    wayfire_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_xwayland_view_base(xww)
    {
        log_info("new xwayland surface %s class: %s instance: %s",
            nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        on_request_move.set_callback([&] (void*) { move_request(); });
        on_request_resize.set_callback([&] (void*) { resize_request(); });
        on_request_maximize.set_callback([&] (void*) {
            maximize_request(xw->maximized_horz && xw->maximized_vert);
        });
        on_request_fullscreen.set_callback([&] (void*) {
            fullscreen_request(output, xw->fullscreen);
        });

        on_set_title.set_callback([&] (void*) { handle_title_changed(); });
        on_set_app_id.set_callback([&] (void*) { handle_app_id_changed(); });
        on_set_parent.set_callback([&] (void*) {
            auto parent = xw->parent ?
                wf_view_from_void(xw->parent->data)->self() : nullptr;
            set_toplevel_parent(parent);
        });

        on_set_title.connect(&xw->events.set_title);
        on_set_app_id.connect(&xw->events.set_class);
        on_set_parent.connect(&xw->events.set_parent);
        on_request_move.connect(&xw->events.request_move);
        on_request_resize.connect(&xw->events.request_resize);
        on_request_maximize.connect(&xw->events.request_maximize);
        on_request_fullscreen.connect(&xw->events.request_fullscreen);

        xw->data = this;
    }

    virtual void destroy()
    {
        on_set_title.disconnect();
        on_set_app_id.disconnect();
        on_set_parent.disconnect();
        on_request_move.disconnect();
        on_request_resize.disconnect();
        on_request_maximize.disconnect();
        on_request_fullscreen.disconnect();

        wayfire_xwayland_view_base::destroy();
    }

    void map(wlr_surface *surface)
    {
        /* override-redirect status changed between creation and MapNotify */
        if (xw->override_redirect)
        {
            auto xsurface = xw; // keep the xsurface in stack, because destroy will likely free this
            destroy();

            auto view = std::make_unique<wayfire_unmanaged_xwayland_view> (xsurface);
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

        auto real_output = output->get_layout_geometry();
        if (!maximized && !fullscreen && !parent)
            move(xw->x - real_output.x, xw->y - real_output.y, false);

        wayfire_view_t::map(surface);
        create_toplevel();
    }

    void commit()
    {
        wayfire_view_t::commit();

        /* Avoid loops where the client wants to have a certain size but the
         * compositor keeps trying to resize it */
        last_server_width = geometry.width;
        last_server_height = geometry.height;
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

        last_server_width = w;
        last_server_height = h;
        send_configure(w, h);
    }

    virtual void request_native_size()
    {
        if (!_is_mapped)
            return;

        if (xw->size_hints->base_width > 0 && xw->size_hints->base_height > 0)
        {
            last_server_width = xw->size_hints->base_width;
            last_server_height = xw->size_hints->base_height;
            send_configure();
        }
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();

        wayfire_view_t::move(g.x, g.y, false);
        resize(g.width, g.height, false);
    }

    void set_maximized(bool maxim)
    {
        wayfire_view_t::set_maximized(maxim);
        wlr_xwayland_surface_set_maximized(xw, maxim);
    }

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
    : wayfire_xwayland_view_base(xww)
{
    log_info("new unmanaged xwayland surface %s class: %s instance: %s",
             nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

    xw->data = this;
    role = WF_VIEW_ROLE_UNMANAGED;
}

void wayfire_unmanaged_xwayland_view::commit()
{
    if (global_x != xw->x || global_y != xw->y)
    {
        geometry.x = global_x = xw->x;
        geometry.y = global_y = xw->y;

        if (output)
        {
            auto real_output = output->get_layout_geometry();
            geometry.x -= real_output.x;
            geometry.y -= real_output.y;
        }

        wayfire_view_t::move(geometry.x, geometry.y, false);
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
    auto wo = core->output_layout->get_output_at(xw->x + surface->current.width / 2, xw->y + surface->current.height / 2);

    if (!wo)
    {
        /* if surface center is outside of anything, try to check the output where the pointer is */
        GetTuple(cx, cy, core->get_cursor_position());
        wo = core->output_layout->get_output_at(cx, cy);
    }

    if (!wo)
        wo = core->get_active_output();
    assert(wo);


    auto real_output_geometry = wo->get_layout_geometry();

    global_x = xw->x;
    global_y = xw->y;
    wayfire_view_t::move(xw->x - real_output_geometry.x,
        xw->y - real_output_geometry.y, false);

    if (wo != output)
    {
        if (output)
            output->workspace->add_view_to_layer(self(), 0);

        set_output(wo);
    }

    damage();

    wayfire_surface_t::map(surface);
    /* We update the keyboard focus before emitting the map event, so that
     * plugins can detect that this view can have keyboard focus */
    _keyboard_focus_enabled = wlr_xwayland_or_surface_wants_focus(xw);

    output->workspace->add_view_to_layer(self(), WF_LAYER_XWAYLAND);
    emit_view_map(self());
    if (wlr_xwayland_or_surface_wants_focus(xw))
    {
        auto wa = output->workspace->get_workarea();
        move(xw->x + wa.x - real_output_geometry.x,
            xw->y + wa.y - real_output_geometry.y, false);

        output->focus_view(self());
    }
}

void wayfire_unmanaged_xwayland_view::unmap()
{
    _is_mapped = false;
    emit_view_unmap(self());
    wayfire_surface_t::unmap();
}

void wayfire_unmanaged_xwayland_view::activate(bool active)
{
    wayfire_view_t::activate(active);
    wlr_xwayland_surface_activate(xw, active);
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

wlr_surface *wayfire_unmanaged_xwayland_view::get_keyboard_focus_surface()
{
    if (wlr_xwayland_or_surface_wants_focus(xw))
        return wayfire_view_t::get_keyboard_focus_surface();

    return nullptr;
}

static wlr_xwayland *xwayland_handle = nullptr;
#endif

void init_xwayland()
{
#if WLR_HAS_XWAYLAND
    static wf::wl_listener_wrapper on_created;
    static signal_callback_t on_shutdown = [&] (void*) {
        wlr_xwayland_destroy(xwayland_handle);
    };

    on_created.set_callback([] (void *data) {
        auto xsurf = (wlr_xwayland_surface*) data;
        if (xsurf->override_redirect) {
            core->add_view(std::make_unique<wayfire_unmanaged_xwayland_view>(xsurf));
        } else {
            core->add_view(std::make_unique<wayfire_xwayland_view> (xsurf));
        }
    });

    xwayland_handle = wlr_xwayland_create(core->display, core->compositor, false);
    if (xwayland_handle)
    {
        on_created.connect(&xwayland_handle->events.new_surface);
        core->connect_signal("shutdown", &on_shutdown);
    }
#endif
}

void xwayland_set_seat(wlr_seat *seat)
{
#if WLR_HAS_XWAYLAND
    if (xwayland_handle)
        wlr_xwayland_set_seat(xwayland_handle, core->get_current_seat());
#endif
}

std::string xwayland_get_display()
{
#if WLR_HAS_XWAYLAND
    return std::to_string(xwayland_handle ? xwayland_handle->display : -1);
#else
    return "-1";
#endif
}
