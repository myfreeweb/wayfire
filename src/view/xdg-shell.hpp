#ifndef XDG_SHELL_HPP
#define XDG_SHELL_HPP

#include "priv-view.hpp"
extern "C"
{
#include <wlr/types/wlr_xdg_shell.h>
}

class wayfire_xdg_popup : public wayfire_surface_t
{
    protected:
    wf::wl_listener_wrapper on_destroy, on_new_popup, on_map, on_unmap;

    wlr_xdg_popup *popup;
    wlr_xdg_surface *xdg_surface;
    void unconstrain();

    public:
    wayfire_xdg_popup(wlr_xdg_popup *popup);
    ~wayfire_xdg_popup();

    virtual void get_child_position(int &x, int &y);
    virtual void get_child_offset(int &x, int &y);

    virtual bool is_subsurface() { return true; }
    virtual void send_done();
};

void create_xdg_popup(wlr_xdg_popup *popup);
class wayfire_xdg_view : public wayfire_view_t
{
    protected:
    wf::wl_listener_wrapper on_map, on_unmap, on_destroy, on_new_popup,
                            on_request_move, on_request_resize,
                            on_request_minimize, on_request_maximize,
                            on_request_fullscreen, on_set_parent,
                            on_set_title, on_set_app_id;

    wf_point xdg_surface_offset = {0, 0};

    public:
    wlr_xdg_surface *xdg_surface;

    wayfire_xdg_view(wlr_xdg_surface *s);
    virtual void map(wlr_surface *surface);

    virtual void get_child_offset(int &x, int &y);

    virtual wf_geometry get_wm_geometry();

    virtual void activate(bool act);
    virtual void set_tiled(uint32_t edges);
    virtual void set_maximized(bool max);
    virtual void set_fullscreen(bool full);
    virtual void move(int w, int h, bool send);
    virtual void resize(int w, int h, bool send);
    virtual void request_native_size();

    virtual void on_xdg_geometry_updated();
    virtual void commit();

    virtual void destroy();

    std::string get_app_id();
    std::string get_title();
    virtual void close();
    ~wayfire_xdg_view();
};

#endif /* end of include guard: XDG_SHELL_HPP */
