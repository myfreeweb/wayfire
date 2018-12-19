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
        wl_listener destroy,
                    new_popup, destroy_popup,
                    m_popup_map, m_popup_unmap;

        wlr_xdg_popup *popup;
        wlr_xdg_surface *xdg_surface;

        void unconstrain();
    public:
        wayfire_xdg_popup(wlr_xdg_popup *popup);
        ~wayfire_xdg_popup();

        virtual void get_child_position(int &x, int &y);
        virtual void get_child_offset(int &x, int &y);

        virtual bool is_subsurface() { return true; }
};

void handle_xdg_new_popup(wl_listener*, void*);

class wayfire_xdg_view : public wayfire_view_t
{
    protected:
        wl_listener destroy_ev, map_ev, unmap_ev, new_popup,
                request_move, request_minimize, request_resize,
                request_maximize, request_fullscreen,
                set_parent_ev, set_title, set_app_id;

        wf_point xdg_surface_offset = {0, 0};

    public:
        wlr_xdg_surface *xdg_surface;

    wayfire_xdg_view(wlr_xdg_surface *s);
    virtual void map(wlr_surface *surface);

    virtual void get_child_offset(int &x, int &y);

    virtual wf_geometry get_wm_geometry();

    virtual void activate(bool act);
    virtual void set_maximized(bool max);
    virtual void set_fullscreen(bool full);
    virtual void move(int w, int h, bool send);
    virtual void resize(int w, int h, bool send);

    virtual void on_xdg_geometry_updated();
    virtual void commit();

    virtual void destroy();

    std::string get_app_id();
    std::string get_title();
    virtual void close();
    ~wayfire_xdg_view();
};

#endif /* end of include guard: XDG_SHELL_HPP */
