#ifndef VIEW_HPP
#define VIEW_HPP
#include <vector>
#include <map>
#include <functional>
#include <nonstd/observer_ptr.h>

#include "plugin.hpp"
#include "opengl.hpp"
#include "object.hpp"
#include "util.hpp"

extern "C"
{
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/util/edges.h>
}

class wayfire_output;

/* General TODO: mark member functions const where appropriate */
class wayfire_view_t;
using wayfire_view = nonstd::observer_ptr<wayfire_view_t>;

/* do not copy the surface, it is not reference counted and you will get crashes */
class wayfire_surface_t;
using wf_surface_iterator_callback = std::function<void(wayfire_surface_t*, int, int)>;

class wf_decorator_frame_t;
class wf_view_transformer_t;

/* abstraction for desktop-apis, no real need for plugins
 * This is a base class to all "drawables" - desktop views, subsurfaces, popups */
class wayfire_surface_t
{
    /* TODO: maybe some functions don't need to be virtual? */
    protected:
        // number of commits to this surface or any child since the surface was created
        int64_t buffer_age = 0;

        wf::wl_listener_wrapper::callback_t handle_new_subsurface;
        wf::wl_listener_wrapper on_commit, on_destroy, on_new_subsurface;
        virtual void for_each_surface_recursive(wf_surface_iterator_callback callback,
                                                int x, int y, bool reverse = false);
        wayfire_output *output = nullptr;

        /* position relative to parent */
        virtual void get_child_position(int &x, int &y);

        wf_geometry geometry = {0, 0, 0, 0};

        virtual bool is_subsurface();
        virtual void damage(const wlr_box& box);
        virtual void damage(const wf_region& region);

        void apply_surface_damage(int x, int y);

        virtual void _wlr_render_box(const wf_framebuffer& fb, int x, int y, const wlr_box& scissor);

        static std::map<std::string, int> shrink_constraints;
        static int maximal_shrink_constraint;

    public:
        /* NOT API */
        wayfire_surface_t *parent_surface;
        /* NOT API */
        std::vector<wayfire_surface_t*> surface_children;

        /* NOT API */
        bool has_client_decoration = true;

        /* offset to be applied for children, NOT API */
        virtual void get_child_offset(int &x, int &y);

        wayfire_surface_t(wayfire_surface_t *parent = nullptr);
        virtual ~wayfire_surface_t();

        /* if surface != nullptr, then the surface is mapped */
        wlr_surface *surface = nullptr;

        /* NOT API */
        virtual void map(wlr_surface *surface);
        /* NOT API */
        virtual void unmap();
        /* NOT API */
        virtual void destruct() { delete this; }

        virtual bool accepts_input(int32_t sx, int32_t sy);
        virtual void send_frame_done(const timespec& now);

        /* Subtract the opaque region of the surface from region, supposing
         * the surface is positioned at (x, y) */
        virtual void subtract_opaque(wf_region& region, int x, int y);

        /* Enforces the opaque region be shrunk by the amount of pixels
         * If multiple plugins request this, the highest constraint is selected,
         * i.e the most shrinking */
        static void set_opaque_shrink_constraint(
            std::string constraint_name, int value);

        virtual wl_client *get_client();

        virtual bool is_mapped();

        virtual wlr_buffer *get_buffer();

        int keep_count = 0;
        bool destroyed = false;

        void inc_keep_count();
        void dec_keep_count();

        virtual wayfire_surface_t *get_main_surface();

        virtual void damage();

        float alpha = 1.0;

        /* transform arg from output-local point to a point relative to the surface,
         * after applying all the surface's parents' transforms */
        virtual wf_point get_relative_position(const wf_point& arg);

        /* returns top-left corner in output coordinates */
        virtual wf_point get_output_position();

        /* NOT API */
        virtual void update_output_position();

        /* return surface box in output coordinates */
        virtual wf_geometry get_output_geometry();

        /* NOT API */
        virtual void commit();

        virtual wayfire_output *get_output() { return output; };
        virtual void set_output(wayfire_output*);

        /* Render the surface at the given coordinates,
         * usually you need render_fb() */
        virtual void simple_render(const wf_framebuffer& fb, int x, int y,
            const wf_region& damage);

        /* Render the surface to the given fb */
        virtual void render_fb(const wf_region& damage, const wf_framebuffer& fb);

        /* iterate all (sub) surfaces, popups, etc. in top-most order
         * for example, first popups, then subsurfaces, then main surface
         * When reverse=true, the order in which surfaces are visited is reversed */
        virtual void for_each_surface(wf_surface_iterator_callback callback,
            bool reverse = false);
};

enum wf_view_role
{
    WF_VIEW_ROLE_TOPLEVEL, // regular, "WM" views
    WF_VIEW_ROLE_UNMANAGED, // xwayland override redirect or unmanaged views
    WF_VIEW_ROLE_SHELL_VIEW // background, lockscreen, panel, notifications, etc
};

/* Represents a desktop window (not as X11 window, but like a xdg_toplevel surface) */
class wayfire_view_t : public wayfire_surface_t, public wf_object_base
{
    protected:

        /* Whether this view is really mapped
         *
         * The only time when this is different from is_mapped() is when the view
         * is about to be unmapped. In this case we no longer have a valid keyboard
         * focus for this view, but the view is still "mapped", e.g. visible */
        bool _is_mapped = false;

        wf::wl_idle_call idle_destruct;

        /* Save the real view layer when it gets fullscreened */
        uint32_t saved_layer = 0;

        /* those two point to the same object. Two fields are used to avoid
         * constant casting to and from types */
        wayfire_surface_t *decoration = NULL;
        wf_decorator_frame_t *frame = NULL;

        void force_update_xwayland_position();
        int in_continuous_move = 0, in_continuous_resize = 0;

        bool wait_decoration = false;
        virtual bool update_size();
        void adjust_anchored_edge(int32_t new_width, int32_t new_height);

        uint32_t id;
        virtual void damage(const wlr_box& box);

        struct offscreen_buffer_t : public wf_framebuffer
        {
            wf_region cached_damage;
            bool valid();
        } offscreen_buffer;

        // the last buffer_age of this view for which the buffer was made
        int64_t last_offscreen_buffer_age = -1;

        struct transform_t : public noncopyable_t
        {
            std::string plugin_name = "";
            std::unique_ptr<wf_view_transformer_t> transform;
            wf_framebuffer fb;

            transform_t();
            ~transform_t();
        };

        wf::safe_list_t<std::shared_ptr<transform_t>> transforms;

        virtual wf_geometry get_untransformed_bounding_box();
        void reposition_relative_to_parent();

        uint32_t edges = 0;

        /* Save the last bounding box on each commit.
         * When the view resizes, some transforms may change the bounding box in such a way that
         * we can't really calculate damage */
        wf_geometry last_bounding_box {0, 0, 0, 0};

        /* Same as damage(), but don't transform box */
        void damage_raw(const wlr_box& box);

        /* The toplevel is created by the individual view's mapping functions,
         * i.e in xdg-shell, xwayland, etc.
         * The handle is automatically destroyed when the view is unmapped */
        wlr_foreign_toplevel_handle_v1 *toplevel_handle = NULL;

        wf::wl_listener_wrapper toplevel_handle_v1_maximize_request,
                                toplevel_handle_v1_activate_request,
                                toplevel_handle_v1_minimize_request,
                                toplevel_handle_v1_set_rectangle_request,
                                toplevel_handle_v1_close_request;

        wlr_box minimize_hint;

        /* Create/destroy the toplevel_handle */
        virtual void create_toplevel();
        virtual void destroy_toplevel();

        /* The following are no-op if toplevel_handle == NULL */
        virtual void toplevel_send_title();
        virtual void toplevel_send_app_id();
        virtual void toplevel_send_state();
        virtual void toplevel_update_output(wayfire_output *output, bool enter);

    public: // NOT API
        /* The handle_{app_id, title}_changed emit the corresponding signal
         * and if there is a toplevel_handle, they send the updated values */
        virtual void handle_app_id_changed();
        virtual void handle_title_changed();
        virtual void handle_minimize_hint(const wlr_box& hint);

        bool _keyboard_focus_enabled = true;
        /* NOT API */
        virtual void set_keyboard_focus_enabled(bool enabled);

    public:
        /* these represent toplevel relations, children here are transient windows,
         * such as the close file dialogue */
        wayfire_view parent = nullptr;
        std::vector<wayfire_view> children;
        virtual void set_toplevel_parent(wayfire_view parent);
        virtual void set_output(wayfire_output*);

        virtual void set_role(wf_view_role new_role);
        wf_view_role role = WF_VIEW_ROLE_TOPLEVEL;

        wayfire_view_t();
        virtual ~wayfire_view_t();
        std::string to_string() const;

        wayfire_view self();

        virtual void move(int x, int y, bool send_signal = true);

        /* both resize and set_geometry just request the client to resize,
         * there is no guarantee that they will actually honour the size.
         * However, maximized surfaces typically do resize to the dimensions
         * they are asked */
        virtual void resize(int w, int h, bool send_signal = true);
        /* Request that the window resizes itself to its preferred size */
        virtual void request_native_size();
        virtual void activate(bool active);
        virtual void close();

        virtual void set_parent(wayfire_view parent);
        virtual wayfire_surface_t* get_main_surface();

        /* return geometry as should be used for all WM purposes */
        virtual wf_geometry get_wm_geometry();

        virtual wf_point get_relative_position(const wf_point& arg);
        virtual wf_point get_output_position();

        /* return the output-local transformed coordinates of the view
         * and all its subsurfaces */
        virtual wlr_box get_bounding_box();

        /* return the output-local transformed coordinates of the view,
         * up to the given transformer. */
        virtual wlr_box get_bounding_box(std::string transformer);
        virtual wlr_box get_bounding_box(
            nonstd::observer_ptr<wf_view_transformer_t> transformer);

        /* transform the given region using the view's transform */
        virtual wlr_box transform_region(const wlr_box &box);

        /* transform the given region using the view's transform
         * up to the given transformer */
        virtual wlr_box transform_region(const wlr_box& box, std::string transformer);
        virtual wlr_box transform_region(const wlr_box& box,
            nonstd::observer_ptr<wf_view_transformer_t> transformer);

        /* check whether the given region intersects any of the surfaces
         * in the view's surface tree. */
        virtual bool intersects_region(const wlr_box& region);

        /* map from global to surface local coordinates
         * returns the (sub)surface under the cursor or NULL iff the cursor is outside of the view
         * TODO: it should be overwritable by plugins which deform the view */
        virtual wayfire_surface_t *map_input_coordinates(int cursor_x, int cursor_y, int &sx, int &sy);

        /* Subtract the opaque region of the surface from region, supposing
         * the surface is positioned at (x, y) */
        virtual void subtract_opaque(wf_region& region, int x, int y);


        /* Returns the wlr_surface which should receive focus if this view is activated */
        virtual wlr_surface *get_keyboard_focus_surface();
        /* Returns whether this view is focuseable. Note that if
         * get_keyboard_focus() returns a non-null surface, the view is
         * focuseable. However, a focuseable view might have a null focus surface,
         * for ex. if it is unmapped */
        virtual bool is_focuseable() const;

        virtual void set_geometry(wf_geometry g);

        /* set edges to control the gravity of resize update
         * default: top-left corner stays where it is */
        virtual void set_resizing(bool resizing, uint32_t edges = 0);
        virtual void set_moving(bool moving);

        bool maximized = false, fullscreen = false, activated = false, minimized = false;
        uint32_t tiled_edges = 0;

        /* Will also move the view to/from the minimized layer, if necessary */
        virtual void set_minimized(bool minimized);
        virtual void set_tiled(uint32_t edges);
        virtual void set_maximized(bool maxim);
        virtual void set_fullscreen(bool fullscreen);

        bool is_visible();
        virtual void commit();
        virtual void map(wlr_surface *surface);
        virtual void unmap();

        /* cleanup of the wf_view part. Not API function */
        virtual void destruct();

        /* cleanup of the wlroots handle. Not API function */
        virtual void destroy();

        virtual void damage();

        virtual std::string get_app_id() { return ""; }
        virtual std::string get_title() { return ""; }

        /* Set if the current view should not be rendered by built-in renderer */
        bool is_hidden = false;

        virtual void move_request();
        virtual void focus_request();
        virtual void resize_request(uint32_t edges = 0);
        virtual void minimize_request(bool state);
        virtual void maximize_request(bool state);
        virtual void fullscreen_request(wayfire_output *output, bool state);

        /* Returns the rectangle on the output to which to minimize towards.
         * Returns a box with width = height = 0 if no hint is set */
        virtual wlr_box get_minimize_hint();

        /* returns whether the view should be decorated */
        virtual bool should_be_decorated();

        /* Used to set a decoration.
         * The parameter object MUST be a subclass of both wayfire_surface_t
         * and of wf_decorator_frame_t
         *
         * The life-time of the decoration ({inc,dec}_keep_count) is managed by the view itself
         * Setting the decoration may change the view output and wm geometry */
        virtual void set_decoration(wayfire_surface_t *frame);

        /* iterate all (sub) surfaces, popups, decorations, etc. in top-most order
         * for example, first popups, then subsurfaces, then main surface
         * When reverse=true, the order in which surfaces are visited is reversed */
        virtual void for_each_surface(wf_surface_iterator_callback callback, bool reverse = false);

        /*
         *                              View transforms
         * A view transform can be any kind of transformation, for example 3D rotation,
         * wobbly effect or similar. When we speak of transforms, a "view" is defined
         * as a toplevel window (including decoration) and also all of its subsurfaces/popups.
         * The transformation then is applied to this group of surfaces together.
         *
         * When a view has a custom transform, then internally all these surfaces are
         * rendered to a FBO, and then the custom transformation renders the resulting
         * texture as it sees fit. In case of multiple transforms, we do multiple render passes
         * where each transform is fed the result of the previous transforms
         *
         * Damage tracking for transformed views is done on the boundingbox of the
         * damaged region after applying the transformation, but all damaged parts
         * of the internal FBO are updated.
         * */

        void add_transformer(std::unique_ptr<wf_view_transformer_t> transformer);

        /* add a transformer with the given name. Note that you can add multiple transforms with the same name!
         * However, get_transformer() and pop_transformer() return only the first transform with the given name */
        void add_transformer(std::unique_ptr<wf_view_transformer_t> transformer, std::string name);

        /* returns NULL if there is no such transform */
        nonstd::observer_ptr<wf_view_transformer_t> get_transformer(std::string name);

        void pop_transformer(nonstd::observer_ptr<wf_view_transformer_t> transformer);
        void pop_transformer(std::string name);

        bool has_transformer();

        virtual void render_fb(const wf_region& damage, const wf_framebuffer& framebuffer);

        bool has_snapshot = false;
        virtual bool can_take_snapshot();
        virtual void take_snapshot();

        /* NOT API */
        int64_t get_buffer_age() { return buffer_age; }
};

wayfire_view wl_surface_to_wayfire_view(wl_resource *surface);
#endif
