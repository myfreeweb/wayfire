#ifndef COMPOSITOR_SURFACE_HPP
#define COMPOSITOR_SURFACE_HPP

#include "view.hpp"

/* This is the base class for compositor-created surfaces
 * They don't have a corresponding client, surface, or buffer,
 * but are rendered from the compositor itself
 *
 * To implement a custom compositor surface, override all the
 * virtual functions marked with "assert(false)" below
 *
 * You might also need to override other functions, depending
 * on the exact use-case you want to achieve. For example,
 * decorations are implemented as subsurfaces - so they need
 * to implement other methods, like get_child_position()
 *
 * */

class wayfire_compositor_surface_t : public wayfire_surface_t
{
    protected:
        virtual void damage(const wlr_box& box) { assert(false); }
        virtual void _wlr_render_box(const wlr_fb_attribs& fb, int x, int y, const wlr_box& scissor) { assert(false); }

    public:
        wayfire_compositor_surface_t() {}
        virtual ~wayfire_compositor_surface_t() {}

        virtual bool is_mapped() { return true; }

        virtual wf_geometry get_output_geometry() { assert(false); return {}; }
        virtual void  render_fb(pixman_region32_t* damage, wf_framebuffer fb) { assert(false); }

        virtual void send_frame_done(const timespec& now) {}

        /* all input events coordinates are surface-local */

        /* override this if you want to get pointer events or to stop input passthrough */
        virtual bool accepts_input(int32_t sx, int32_t sy) { return false; }

        virtual void on_pointer_enter(int x, int y) {}
        virtual void on_pointer_leave() {}
        virtual void on_pointer_motion(int x, int y) {}
        virtual void on_pointer_button(uint32_t button, uint32_t state) {}

        // XXX: supports only single touch
        virtual void on_touch_down(int x, int y) {}
        virtual void on_touch_up() {}
        virtual void on_touch_motion(int x, int y) {}
};

static wayfire_compositor_surface_t *wf_compositor_surface_from_surface(wayfire_surface_t *surface)
{
    return dynamic_cast<wayfire_compositor_surface_t*> (surface);
}

/* TODO: implement compositor views - needs a real use-case */

#endif /* end of include guard: COMPOSITOR_SURFACE_HPP */
