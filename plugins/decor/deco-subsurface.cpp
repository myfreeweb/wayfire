extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_xcursor_manager.h>
#undef static
}

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

#include <linux/input-event-codes.h>

#include <compositor-surface.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <core.hpp>
#include <debug.hpp>
#include <decorator.hpp>
#include <view-transform.hpp>
#include <signal-definitions.hpp>
#include "deco-subsurface.hpp"

#include <cairo.h>

const int titlebar_thickness = 30;
const int resize_edge_threshold = 5;
const int normal_thickness = resize_edge_threshold;

GLuint get_text_texture(int width, int height, std::string text)
{
    const auto format = CAIRO_FORMAT_ARGB32;
    auto surface = cairo_image_surface_create(format, width, height);
    auto cr = cairo_create(surface);

    const float font_scale = 0.8;
    const float font_size = height * font_scale;

    // render text
    cairo_select_font_face(cr, "serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);

    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);

    cairo_move_to(cr, normal_thickness, font_size);
    cairo_show_text(cr, text.c_str());

    cairo_destroy(cr);

    auto src = cairo_image_surface_get_data(surface);

    GLuint tex;
    GL_CALL(glGenTextures(1, &tex));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, src));

    cairo_surface_destroy(surface);

    return tex;
}

class simple_decoration_surface : public wayfire_compositor_surface_t, public wf_decorator_frame_t
{
    int thickness = normal_thickness;
    int titlebar = titlebar_thickness;

    wayfire_view view;
    signal_callback_t title_set;

    protected:
        virtual void damage(const wlr_box& box)
        {
            assert(false);
        }

        virtual void get_child_position(int &x, int &y)
        {
            view->get_child_offset(x, y);
            x -= thickness;
            y -= titlebar;
        }

    public:
        simple_decoration_surface(wayfire_view view)
        {
            this->view = view;
            title_set = [=] (signal_data *data)
            {
                if (get_signaled_view(data) == view)
                    notify_view_resized(view->get_wm_geometry());
            };
        }

        virtual void set_output(wayfire_output *next_output)
        {
            if (this->output)
                this->output->disconnect_signal("view-title-changed", &title_set);

            wayfire_compositor_surface_t::set_output(next_output);

            if (this->output)
                this->output->connect_signal("view-title-changed", &title_set);
        }

        virtual ~simple_decoration_surface()
        {
        }

        virtual bool is_mapped()
        {
            return true;
        }

        const int text_field_width = 500;
        int width = 100, height = 100;
        virtual wf_geometry get_output_geometry()
        {
            auto pos = get_output_position();
            return {pos.x, pos.y, width, height};
        }

        bool active = true; // when views are mapped, they are usually activated
        float border_color[4] = {0.15f, 0.15f, 0.15f, 0.8f};
        float border_color_inactive[4] = {0.25f, 0.25f, 0.25f, 0.95f};

        GLuint tex = -1;

        virtual void _wlr_render_box(const wlr_fb_attribs& fb, int x, int y, const wlr_box& scissor)
        {
            wlr_box geometry {x, y, width, height};
            geometry = get_output_box_from_box(geometry, output->handle->scale);

            float projection[9];
            wlr_matrix_projection(projection, fb.width, fb.height, fb.transform);

            float matrix[9];
            wlr_matrix_project_box(matrix, &geometry, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

            wlr_renderer_begin(core->renderer, fb.width, fb.height);
            auto sbox = scissor; wlr_renderer_scissor(core->renderer, &sbox);

            wlr_render_quad_with_matrix(core->renderer, active ? border_color : border_color_inactive, matrix);

            if (tex == (uint)-1)
                tex = get_text_texture(geometry.width, titlebar, view->get_title());

            auto ortho = glm::ortho(0.0f, 1.0f * fb.width, 1.0f * fb.height, 0.0f);

            gl_geometry gg;
            gg.x1 = geometry.x;
            gg.y1 = geometry.y;
            gg.x2 = geometry.x + geometry.width;
            gg.y2 = geometry.y + titlebar;

            OpenGL::use_default_program();
            OpenGL::render_transformed_texture(tex, gg, {}, ortho, {1, 1, 1, 1}, TEXTURE_TRANSFORM_INVERT_Y);

            wlr_renderer_end(core->renderer);
        }

        virtual void _render_pixman(const wlr_fb_attribs& fb, int x, int y, pixman_region32_t* damage)
        {
            const float scale = output->handle->scale;

            pixman_region32_t frame_region;
            pixman_region32_init(&frame_region);

            /* top */
            pixman_region32_union_rect(&frame_region, &frame_region, x * scale, y * scale, width * scale, titlebar * scale);
            /* left */
            pixman_region32_union_rect(&frame_region, &frame_region, x * scale, y * scale, thickness * scale, height * scale);
            /* right */
            pixman_region32_union_rect(&frame_region, &frame_region, x * scale + (width - thickness) * scale, y * scale,
                                       thickness * scale, height * scale);
            /* bottom */
            pixman_region32_union_rect(&frame_region, &frame_region, x * scale, y * scale + (height - thickness) * scale,
                                       width * scale, thickness * scale);

            pixman_region32_intersect(&frame_region, &frame_region, damage);
            wayfire_surface_t::_render_pixman(fb, x, y, &frame_region);
            pixman_region32_fini(&frame_region);
        }

        virtual void render_fb(pixman_region32_t* damage, wf_framebuffer fb)
        {
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.fb));
            auto obox = get_output_geometry();

            wlr_fb_attribs attribs;
            attribs.width = output->handle->width;
            attribs.height = output->handle->height;
            attribs.transform = output->handle->transform;

            render_pixman(attribs, obox.x - fb.geometry.x, obox.y - fb.geometry.y, damage);
        }

        /* all input events coordinates are surface-local */
        virtual bool accepts_input(int32_t sx, int32_t sy)
        {
            /* outside of the decoration + view */
            if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                return false;

            /* inside the frame */
            bool is_horiz = true, is_vert = true;
            if (thickness < sx && sx < width - thickness)
                is_horiz = false;

            if (titlebar < sy && sy < height - thickness)
                is_vert = false;

            return is_horiz || is_vert;
        }

        virtual void on_pointer_enter(int x, int y)
        {
            cursor_x = x;
            cursor_y = y;

            update_cursor();
        }

        virtual void on_pointer_leave()
        { }

        int cursor_x, cursor_y;
        virtual void on_pointer_motion(int x, int y)
        {
            cursor_x = x;
            cursor_y = y;

            update_cursor();
        }

        void send_move_request()
        {
            move_request_signal move_request;
            move_request.view = view;
            output->emit_signal("move-request", &move_request);
        }

        void send_resize_request(int x, int y)
        {
            resize_request_signal resize_request;
            resize_request.view = view;
            resize_request.edges = get_edges(x, y);
            output->emit_signal("resize-request", &resize_request);
        }

        uint32_t get_edges(int x, int y)
        {
            uint32_t edges = 0;
            if (x <= thickness)
                edges |= WLR_EDGE_LEFT;
            if (x >= width - thickness)
                edges |= WLR_EDGE_RIGHT;
            if (y <= thickness)
                edges |= WLR_EDGE_TOP;
            if (y >= height - thickness)
                edges |= WLR_EDGE_BOTTOM;

            return edges;
        }

        std::string get_cursor(uint32_t edges)
        {
            if (edges)
                return wlr_xcursor_get_resize_name((wlr_edges) edges);
            return "default";
        }

        void update_cursor()
        {
            core->set_cursor(get_cursor(get_edges(cursor_x, cursor_y)));
        }

        virtual void on_pointer_button(uint32_t button, uint32_t state)
        {
            if (button != BTN_LEFT || state != WLR_BUTTON_PRESSED)
                return;

            if (get_edges(cursor_x, cursor_y))
                return send_resize_request(cursor_x, cursor_y);
            send_move_request();
        }

        virtual void on_touch_down(int x, int y)
        {
            if (get_edges(x, y))
                return send_resize_request(x, y);

            send_move_request();
        }

        /* frame implementation */
        virtual wf_geometry expand_wm_geometry(wf_geometry contained_wm_geometry)
        {
            contained_wm_geometry.x -= thickness;
            contained_wm_geometry.y -= titlebar;
            contained_wm_geometry.width += 2 * thickness;
            contained_wm_geometry.height += thickness + titlebar;

            return contained_wm_geometry;
        }

        virtual void calculate_resize_size(int& target_width, int& target_height)
        {
            target_width -= 2 * thickness;
            target_height -= thickness + titlebar;

            target_width = std::max(target_width, 1);
            target_height = std::max(target_height, 1);
        }

        virtual void notify_view_activated(bool active)
        {
            if (this->active != active)
                view->damage();

            this->active = active;
        }

        virtual void notify_view_resized(wf_geometry view_geometry)
        {
            if (tex != (uint32_t)-1)
                GL_CALL(glDeleteTextures(1, &tex));
            tex = -1;

            width = view_geometry.width;
            height = view_geometry.height;

            view->damage();
        };

        virtual void notify_view_maximized()
        {

        }

        virtual void notify_view_fullscreened()
        {
            if (view->fullscreen)
            {
                thickness = 0;
                titlebar = 0;
                view->resize(width, height);
            } else
            {
                thickness = normal_thickness;
                titlebar = titlebar_thickness;
                view->resize(width, height);
            }
        };
};

void init_view(wayfire_view view)
{
    auto surf = new simple_decoration_surface(view);
    view->set_decoration(surf);
    view->damage();
}
