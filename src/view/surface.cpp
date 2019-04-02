#include <algorithm>
extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/region.h>
#undef static
}

#include "priv-view.hpp"
#include "opengl.hpp"
#include "core.hpp"
#include "output.hpp"
#include "debug.hpp"
#include "render-manager.hpp"
#include "signal-definitions.hpp"

wayfire_surface_t::wayfire_surface_t(wayfire_surface_t* parent)
{
    inc_keep_count();
    this->parent_surface = parent;

    if (parent)
    {
        set_output(parent->output);
        parent->surface_children.push_back(this);
    }

    handle_new_subsurface = [&] (void* data)
    {
        auto sub = static_cast<wlr_subsurface*> (data);
        if (sub->surface->data)
            return;

        auto it = std::find_if(surface_children.begin(), surface_children.end(),
            [=] (wayfire_surface_t *surface) { return surface->surface == sub->surface; });

        if (it == surface_children.end())
        {
            auto surf = new wayfire_surface_t(this);
            surf->map(sub->surface);
        } else
        {
            log_error("adding same subsurface twice!");
        }
    };

    on_new_subsurface.set_callback(handle_new_subsurface);
    on_commit.set_callback([&] (void*) { commit(); });
}

wayfire_surface_t::~wayfire_surface_t()
{
    if (parent_surface)
    {
        auto it = parent_surface->surface_children.begin();
        while(it != parent_surface->surface_children.end())
        {
            if (*it == this)
                it = parent_surface->surface_children.erase(it);
            else
                ++it;
        }
    }

    for (auto c : surface_children)
        c->parent_surface = nullptr;
}

wayfire_surface_t *wayfire_surface_t::get_main_surface()
{
    return this->parent_surface ? this->parent_surface->get_main_surface() : this;
}

bool wayfire_surface_t::is_subsurface()
{
    return wlr_surface_is_subsurface(surface);
}

void wayfire_surface_t::get_child_position(int &x, int &y)
{
    auto sub = wlr_subsurface_from_wlr_surface(surface);
    assert(sub);

    x = sub->current.x;
    y = sub->current.y;
}

void wayfire_surface_t::get_child_offset(int &x, int &y)
{
    x = 0;
    y = 0;
};

void wayfire_surface_t::send_frame_done(const timespec& time)
{
    wlr_surface_send_frame_done(surface, &time);
}

bool wayfire_surface_t::accepts_input(int32_t sx, int32_t sy)
{
    if (!surface)
        return false;

    return wlr_surface_point_accepts_input(surface, sx, sy);
}

int wayfire_surface_t::maximal_shrink_constraint = 0;
std::map<std::string, int> wayfire_surface_t::shrink_constraints;
void wayfire_surface_t::set_opaque_shrink_constraint(std::string name, int value)
{
    shrink_constraints[name] = value;

    maximal_shrink_constraint = 0;
    for (auto& constr : shrink_constraints)
    {
        maximal_shrink_constraint =
            std::max(maximal_shrink_constraint, constr.second);
    }
}

void wayfire_surface_t::subtract_opaque(wf_region& region, int x, int y)
{
    if (!surface)
        return;

    wf_region opaque{&surface->current.opaque};
    opaque += wf_point{x, y};
    opaque *= output->handle->scale;
    /* region scaling uses std::ceil/std::floor, so the resulting region
     * encompasses the opaque region. However, in the case of opaque region, we
     * don't want any pixels that aren't actually opaque. So in case of different
     * scales, we just shrink by 1 to compensate for the ceil/floor discrepancy */
    int ceil_factor = 0;
    if (output->handle->scale != (float)surface->current.scale)
        ceil_factor = 1;

    opaque.expand_edges(-maximal_shrink_constraint - ceil_factor);
    region ^= opaque;
}

wl_client* wayfire_surface_t::get_client()
{
    if (surface)
        return wl_resource_get_client(surface->resource);

    return nullptr;
}

bool wayfire_surface_t::is_mapped()
{
    return !destroyed && surface;
}

wf_point wayfire_surface_t::get_relative_position(const wf_point& arg)
{
    if (!is_mapped())
        return arg;

    wf_point result = arg;

    /* The root of each surface tree is a view */
    auto view = dynamic_cast<wayfire_view_t*> (get_main_surface());
    assert(view);

    auto transformed = view->get_relative_position(result);

    auto output_position = get_output_position();
    auto view_position = view->get_output_position();

    return {transformed.x + view_position.x - output_position.x,
        transformed.y + view_position.y - output_position.y};
}

wf_point wayfire_surface_t::get_output_position()
{
    auto pos = parent_surface->get_output_position();

    int dx, dy;
    get_child_position(dx, dy);
    pos.x += dx; pos.y += dy;

    return pos;
}

wf_geometry wayfire_surface_t::get_output_geometry()
{
    if (!is_mapped())
        return geometry;

    auto pos = get_output_position();
    return {
        pos.x, pos.y,
        surface->current.width,
        surface->current.height
    };
}

void emit_map_state_change(wayfire_surface_t *surface)
{
    std::string state = surface->is_mapped() ? "_surface_mapped" : "_surface_unmapped";

    _surface_map_state_changed_signal data;
    data.surface = surface;
    if (surface->get_output())
        surface->get_output()->emit_signal(state, &data);

    core->emit_signal(state, &data);
}

void wayfire_surface_t::map(wlr_surface *surface)
{
    assert(!this->surface && surface);
    this->surface = surface;

    /* force surface_send_enter() */
    set_output(output);

    on_new_subsurface.connect(&surface->events.new_subsurface);
    on_commit.connect(&surface->events.commit);

    /* map by default if this is a subsurface, only toplevels/popups have map/unmap events */
    if (wlr_surface_is_subsurface(surface))
    {
        auto sub = wlr_subsurface_from_wlr_surface(surface);
        on_destroy.set_callback([&] (void*) { destroyed = 1; unmap(); dec_keep_count(); });
        on_destroy.connect(&sub->events.destroy);
    }

    surface->data = this;
    damage();

    wlr_subsurface *sub;
    wl_list_for_each(sub, &surface->subsurfaces, parent_link)
        handle_new_subsurface(sub);

    if (core->uses_csd.count(surface))
        this->has_client_decoration = core->uses_csd[surface];

    if (is_subsurface())
        emit_map_state_change(this);
}

void wayfire_surface_t::unmap()
{
    assert(this->surface);
    damage();

    this->surface->data = NULL;
    this->surface = nullptr;
    emit_map_state_change(this);

    on_new_subsurface.disconnect();
    on_destroy.disconnect();
    on_commit.disconnect();
}

wlr_buffer* wayfire_surface_t::get_buffer()
{
    if (surface && wlr_surface_has_buffer(surface))
        return surface->buffer;

    return nullptr;
}

void wayfire_surface_t::inc_keep_count()
{
    ++keep_count;
}

void wayfire_surface_t::dec_keep_count()
{
    --keep_count;
    if (!keep_count)
        destruct();
}

void wayfire_surface_t::damage(const wf_region& dmg)
{
    for (const auto& rect : dmg)
        damage(wlr_box_from_pixman_box(rect));
}

void wayfire_surface_t::damage(const wlr_box& box)
{
    if (parent_surface)
        parent_surface->damage(box);
}

void wayfire_surface_t::damage()
{
    /* TODO: bounding box damage */
    damage(geometry);
}

void wayfire_surface_t::update_output_position()
{
    wf_geometry rect = get_output_geometry();

    /* TODO: recursively damage children? */
    if (is_subsurface() && rect != geometry)
    {
        damage(geometry);
        damage(rect);

        geometry = rect;
    }
}

void wayfire_surface_t::apply_surface_damage(int x, int y)
{
    if (!output || !is_mapped())
        return;

    wf_region dmg;
    wlr_surface_get_effective_damage(surface, dmg.to_pixman());

    if (surface->current.scale != 1 ||
        surface->current.scale != output->handle->scale)
        dmg.expand_edges(1);

    dmg += wf_point{x, y};
    damage(dmg);
}

void wayfire_surface_t::commit()
{
    update_output_position();
    auto pos = get_output_position();
    apply_surface_damage(pos.x, pos.y);

    if (output)
    {
        /* we schedule redraw, because the surface might expect
         * a frame callback */
        output->render->schedule_redraw();
    }

    buffer_age++;
    auto parent = this->parent_surface;
    while (parent)
    {
        parent->buffer_age++;
        parent = parent->parent_surface;
    }
}

void wayfire_surface_t::set_output(wayfire_output *out)
{
    if (output && surface)
        wlr_surface_send_leave(surface, output->handle);

    if (out && surface)
        wlr_surface_send_enter(surface, out->handle);

    output = out;
    for (auto c : surface_children)
        c->set_output(out);
}

void wayfire_surface_t::for_each_surface_recursive(wf_surface_iterator_callback call,
                                                   int x, int y, bool reverse)
{
    if (!is_mapped())
        return;

    auto handle_child = [&] (wayfire_surface_t *child)
    {
        if (!child->is_mapped())
            return;

        int dx, dy;
        child->get_child_position(dx, dy);
        child->for_each_surface_recursive(call, x + dx, y + dy, reverse);
    };

    if (reverse)
    {
        call(this, x, y);
        for (auto& c : surface_children)
            handle_child(c);
    } else
    {
        for (auto& c : wf::reverse(surface_children))
            handle_child(c);
        call(this, x, y);
    }
}

void wayfire_surface_t::for_each_surface(wf_surface_iterator_callback call, bool reverse)
{
    auto pos = get_output_position();
    for_each_surface_recursive(call, pos.x, pos.y, reverse);
}

void wayfire_surface_t::_wlr_render_box(const wf_framebuffer& fb, int x, int y, const wlr_box& scissor)
{
    if (!get_buffer())
        return;

    wlr_box geometry {x, y, surface->current.width, surface->current.height};
    geometry = fb.damage_box_from_geometry_box(geometry);

    float projection[9];
    wlr_matrix_projection(projection, fb.viewport_width, fb.viewport_height,
        (wl_output_transform)fb.wl_transform);

    float matrix[9];
    wlr_matrix_project_box(matrix, &geometry, wlr_output_transform_invert(surface->current.transform),
                           0, projection);

    OpenGL::render_begin(fb);
    auto sbox = scissor; wlr_renderer_scissor(core->renderer, &sbox);
    wlr_render_texture_with_matrix(core->renderer, get_buffer()->texture, matrix, alpha);

#ifdef WAYFIRE_GRAPHICS_DEBUG
    float scissor_proj[9];
    wlr_matrix_projection(scissor_proj, fb.viewport_width, fb.viewport_height,
        WL_OUTPUT_TRANSFORM_NORMAL);

    float col[4] = {0, 0.2, 0, 0.5};
    wlr_render_rect(core->renderer, &scissor, col, scissor_proj);
#endif

    OpenGL::render_end();
}

void wayfire_surface_t::simple_render(const wf_framebuffer& fb, int x, int y, const wf_region& damage)
{
    for (const auto& rect : damage)
    {
        auto box = wlr_box_from_pixman_box(rect);
        _wlr_render_box(fb, x, y, fb.framebuffer_box_from_damage_box(box));
    }
}

void wayfire_surface_t::render_fb(const wf_region& damage, const wf_framebuffer& fb)
{
    if (!is_mapped() || !wlr_surface_has_buffer(surface))
        return;

    auto obox = get_output_geometry();
    simple_render(fb, obox.x - fb.geometry.x, obox.y - fb.geometry.y, damage);
}
