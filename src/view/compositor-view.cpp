#include "priv-view.hpp"
#include "core.hpp"
#include "output.hpp"
#include "opengl.hpp"
#include "workspace-manager.hpp"
#include "compositor-view.hpp"
#include "debug.hpp"

#include <glm/gtc/matrix_transform.hpp>

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#undef static
}

wayfire_compositor_view_t::wayfire_compositor_view_t()
{
    role = WF_VIEW_ROLE_TOPLEVEL;
}

wf_point wayfire_compositor_view_t::get_output_position()
{
    return {geometry.x, geometry.y};
}

wf_geometry wayfire_compositor_view_t::get_output_geometry()
{
    return geometry;
}

wf_geometry wayfire_compositor_view_t::get_wm_geometry()
{
    return geometry;
}

void wayfire_compositor_view_t::set_geometry(wf_geometry g)
{
    damage();
    geometry = g;
    damage();
}

void wayfire_compositor_view_t::map()
{
    if (_is_mapped)
        return;
    _is_mapped = true;

    output->attach_view(self());
    emit_view_map(self());
    emit_map_state_change(this);
}

void wayfire_compositor_view_t::unmap()
{
    if (!_is_mapped)
        return;

    if (output)
        emit_view_unmap(self());

    _is_mapped = false;

    if (output)
        emit_map_state_change(this);
}

void wayfire_compositor_view_t::close()
{
    unmap();
    destroy();
}

void wayfire_compositor_view_t::render_fb(const wf_region& damage, const wf_framebuffer& fb)
{
    /* We only want to circumvent wayfire_surface_t::render_fb,
     * because it relies on the surface having a buffer
     *
     * If there is a transform though, we're fine with what wayfire_view_t provides */
    if (has_transformer())
        return wayfire_view_t::render_fb(damage, fb);

    auto obox = get_output_geometry();
    simple_render(fb, obox.x - fb.geometry.x, obox.y - fb.geometry.y, damage);
}

void wayfire_mirror_view_t::_wlr_render_box(const wf_framebuffer& fb,
        int x, int y, const wlr_box& scissor)
{
    /* Do nothing, we draw directly in simple_render() */
    assert(false);
}

void wayfire_mirror_view_t::simple_render(const wf_framebuffer& fb, int x, int y, const wf_region& damage)
{
    assert(original_view);
    original_view->simple_render(fb, x, y, damage);
}

void wayfire_mirror_view_t::render_fb(const wf_region& damage, const wf_framebuffer& fb)
{
    /* If we have transformers, we're fine. take_snapshot will do the things needed */
    if (has_transformer())
        return wayfire_compositor_view_t::render_fb(damage, fb);

    /* The base view is in another coordinate system,
     * we need to calculate the difference between * the two,
     * so that it appears at the correct place
     *
     * Note that this simply means we need to change fb. Damage is
     * calculated for this mirror_view, and needs to stay as it is */
    auto base_bounding_box = original_view->get_bounding_box();
    auto bbox = get_bounding_box();

    /* Normally we shouldn't copy framebuffers. But in this case we can assume
     * nothing will break, because the copy will be destroyed immediately */
    wf_framebuffer copy;
    memcpy((void*)&copy, (void*)&fb, sizeof(wf_framebuffer));
    copy.geometry.x += base_bounding_box.x - bbox.x;
    copy.geometry.y += base_bounding_box.y - bbox.y;

    original_view->render_fb(damage, copy);

    copy.reset();
}

wayfire_mirror_view_t::wayfire_mirror_view_t(wayfire_view original_view)
{
    this->original_view = original_view;

    base_view_unmapped = [=] (signal_data*)
    {
        /* If the view was mapped, then the base view unmapping is what triggers
         * its destructions. Otherwise, just unset the original view */
        if (is_mapped())
        {
            unmap();
            unset_original_view();
            destroy();
        } else
        {
            unset_original_view();
        }
    };

    original_view->connect_signal("unmap", &base_view_unmapped);

    base_view_damaged = [=] (signal_data* ) {
        damage();
        if (frame)
            frame->notify_view_resized(get_wm_geometry());
    };

    original_view->connect_signal("damaged-region", &base_view_damaged);
}

wayfire_mirror_view_t::~wayfire_mirror_view_t()
{
    if (original_view)
        unset_original_view();
}

bool wayfire_mirror_view_t::can_take_snapshot()
{
    return original_view.get();
}

void wayfire_mirror_view_t::take_snapshot()
{
    if (!can_take_snapshot())
        return;

    auto obox = original_view->get_bounding_box();
    auto buffer_geometry = get_untransformed_bounding_box();

    geometry.width = obox.width;
    geometry.height = obox.height;

    float scale = output ? output->handle->scale : 1;
    int scaled_width = scale * obox.width;
    int scaled_height = scale * obox.height;

    offscreen_buffer.geometry = buffer_geometry;
    offscreen_buffer.scale = scale;

    OpenGL::render_begin();
    offscreen_buffer.allocate(scaled_width, scaled_height);
    offscreen_buffer.bind();
    OpenGL::clear({0, 0, 0, 0});
    OpenGL::render_end();

    wf_framebuffer fb;
    fb.fb = offscreen_buffer.fb;
    fb.tex = offscreen_buffer.tex;

    fb.geometry = obox;
    fb.viewport_width = scaled_width;
    fb.viewport_height = scaled_height;
    fb.scale = scale;

    original_view->render_fb(fb.get_damage_region(), fb);
}

wf_point wayfire_mirror_view_t::get_output_position()
{
    return wayfire_compositor_view_t::get_output_position();
}

wf_geometry wayfire_mirror_view_t::get_output_geometry()
{
    if (!original_view)
        return wayfire_compositor_view_t::get_output_geometry();

    auto bbox = original_view->get_bounding_box();
    geometry.width = bbox.width;
    geometry.height = bbox.height;

    return geometry;
}

wf_geometry wayfire_mirror_view_t::get_wm_geometry()
{
    if (!original_view)
        return wayfire_compositor_view_t::get_wm_geometry();

    auto bbox = original_view->get_bounding_box();
    geometry.width = bbox.width;
    geometry.height = bbox.height;

    return geometry;
}

wf_geometry wayfire_mirror_view_t::get_untransformed_bounding_box()
{
    if (original_view)
        return get_output_geometry();

    return wayfire_compositor_view_t::get_untransformed_bounding_box();
}

void wayfire_mirror_view_t::unset_original_view()
{
    original_view->disconnect_signal("damaged-region", &base_view_damaged);
    original_view->disconnect_signal("unmap", &base_view_unmapped);

    original_view = nullptr;
}

void wayfire_mirror_view_t::unmap()
{
    wayfire_compositor_view_t::unmap();
}

void wayfire_color_rect_view_t::_render_rect(const wf_framebuffer& fb,
    float projection[9], int x, int y, int w, int h, const wf_color& color)
{
    wlr_box render_geometry_unscaled {x, y, w, h};
    wlr_box render_geometry = fb.damage_box_from_geometry_box(
        render_geometry_unscaled);

    float matrix[9];
    wlr_matrix_project_box(matrix, &render_geometry, WL_OUTPUT_TRANSFORM_NORMAL, 0, projection);

    float col[4] = {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
    wlr_render_quad_with_matrix(core->renderer, col, matrix);
}

void wayfire_color_rect_view_t::_wlr_render_box(const wf_framebuffer& fb, int x, int y, const wlr_box& scissor)
{
    float projection[9];
    wlr_matrix_projection(projection, fb.viewport_width, fb.viewport_height,
        (wl_output_transform)fb.wl_transform);

    OpenGL::render_begin(fb);
    auto sbox = scissor; wlr_renderer_scissor(core->renderer, &sbox);

    /* Draw the border, making sure border parts don't overlap, otherwise
     * we will get wrong corners if border has alpha != 1.0 */
    // top
    _render_rect(fb, projection, x, y, geometry.width, border,
        _border_color);
    // bottom
    _render_rect(fb, projection, x, y + geometry.height - border,
        geometry.width, border, _border_color);
    // left
    _render_rect(fb, projection, x, y + border, border, geometry.height - 2 * border,
        _border_color);
    // right
    _render_rect(fb, projection, x + geometry.width - border, y + border, border,
        geometry.height - 2 * border, _border_color);

    /* Draw the inside of the rect */
    _render_rect(fb, projection, x + border, y + border,
        geometry.width - 2 * border, geometry.height - 2 * border,
        _color);

    OpenGL::render_end();
}

wayfire_color_rect_view_t::wayfire_color_rect_view_t()
{
    set_color({0, 0, 0, 0.5});
    set_border_color({1, 1, 1, 1});
    set_border(20);
}

void wayfire_color_rect_view_t::set_color(wf_color color)
{
    _color = color;
    damage();
}

void wayfire_color_rect_view_t::set_border_color(wf_color border)
{
    _border_color = border;
    damage();
}

void wayfire_color_rect_view_t::set_border(int width)
{
    border = width;
    damage();
}
