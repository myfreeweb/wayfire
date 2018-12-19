#ifndef XDG_SHELL_V6_HPP
#define XDG_SHELL_V6_HPP

#include "priv-view.hpp"
extern "C"
{
#include <wlr/types/wlr_xdg_shell_v6.h>
}

class wayfire_xdg6_popup : public wayfire_surface_t
{
    protected:
        wl_listener destroy,
                    new_popup, destroy_popup,
                    m_popup_map, m_popup_unmap;

        wlr_xdg_popup_v6 *popup;
        wlr_xdg_surface_v6 *xdg_surface;

        void unconstrain();

    public:
        wayfire_xdg6_popup(wlr_xdg_popup_v6 *popup);
        ~wayfire_xdg6_popup();

        virtual void get_child_position(int &x, int &y);
        virtual bool is_subsurface() { return true; }
};

class wayfire_xdg6_view : public wayfire_view_t
{
    protected:
        wl_listener destroy_ev, map_ev, unmap_ev, new_popup,
                request_move, request_resize,
                request_maximize, request_minimize, request_fullscreen,
                set_title, set_app_id;

        wf_point xdg_surface_offset = {0, 0};
    public:
        wl_listener    set_parent_ev;
        wlr_xdg_surface_v6 *v6_surface;

        virtual void get_child_offset(int& x, int& y);

        wayfire_xdg6_view(wlr_xdg_surface_v6 *s);
        virtual void map(wlr_surface *surface);
        virtual void activate(bool act);
        virtual void set_maximized(bool max);
        virtual void set_fullscreen(bool full);
        virtual void move(int w, int h, bool send);
        virtual void resize(int w, int h, bool send);
        virtual wf_geometry get_wm_geometry();
        virtual void commit();

        std::string get_app_id();
        std::string get_title();
        virtual void close();
        virtual void destroy();
        ~wayfire_xdg6_view();
};

#endif /* end of include guard: XDG_SHELL_V6_HPP */
