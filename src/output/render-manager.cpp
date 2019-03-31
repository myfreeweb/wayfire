#include "render-manager.hpp"
#include "output.hpp"
#include "core.hpp"
#include "workspace-manager.hpp"
#include "../core/seat/input-manager.hpp"
#include "opengl.hpp"
#include "debug.hpp"
#include "../main.hpp"
#include <algorithm>

extern "C"
{
    /* wlr uses some c99 extensions, we "disable" the static keyword to workaround */
#define static
#include <wlr/render/wlr_renderer.h>
#undef static
#include <wlr/types/wlr_output_damage.h>
#include <wlr/util/region.h>
}

#include "view/priv-view.hpp"

struct wf_output_damage
{
    wf_region frame_damage;
    wlr_output *output;
    wlr_output_damage *damage_manager;

    wf_output_damage(wlr_output *output)
    {
        this->output = output;
        damage_manager = wlr_output_damage_create(output);
    }

    void add()
    {
        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);
        add({0, 0, w, h});
    }

    void add(const wlr_box& box)
    {
        frame_damage |= box;

        auto sbox = box;
        wlr_output_damage_add_box(damage_manager, &sbox);
        schedule_repaint();
    }

    void add(const wf_region& region)
    {
        frame_damage |= region;
        wlr_output_damage_add(damage_manager,
            const_cast<wf_region&> (region).to_pixman());
        schedule_repaint();
    }

    bool make_current(wf_region& out_damage, bool& need_swap)
    {
        auto r = wlr_output_damage_make_current(damage_manager, &need_swap,
            out_damage.to_pixman());
        if (r)
        {
            int w, h;
            wlr_output_transformed_resolution(output, &w, &h);
            wlr_box whole_output = wlr_box{0, 0, w, h};

            frame_damage ^= whole_output;
            out_damage |= frame_damage;

            if (runtime_config.no_damage_track)
                out_damage |= whole_output;
        }
        return r;
    }

    void swap_buffers(timespec *when, wf_region& swap_damage)
    {
        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);

        wl_output_transform transform =
            wlr_output_transform_invert(output->transform);
        wlr_region_transform(swap_damage.to_pixman(), swap_damage.to_pixman(),
            transform, w, h);

        wlr_output_damage_swap_buffers(damage_manager, when,
            const_cast<wf_region&> (swap_damage).to_pixman());
        frame_damage.clear();
    }

    void schedule_repaint()
    {
        wlr_output_schedule_frame(output);
    }
};

render_manager::render_manager(wayfire_output *o)
{
    output = o;

    /* TODO: do we really need a unique_ptr? */
    output_damage = std::unique_ptr<wf_output_damage>(new wf_output_damage(output->handle));
    output_damage->add();

    on_frame.set_callback([&] (void*) { paint(); });
    on_frame.connect(&output_damage->damage_manager->events.frame);

    init_default_streams();
    schedule_redraw();
}

void render_manager::init_default_streams()
{
    /* We use core->vwidth/vheight directly because it is likely workspace_manager
     * hasn't been initialized yet */
    output_streams.resize(core->vwidth);
    for (int i = 0; i < core->vwidth; i++)
    {
        for (int j = 0; j < core->vheight; j++)
        {
            output_streams[i].push_back(wf_workspace_stream{});
            output_streams[i][j].buffer.fb = output_streams[i][j].buffer.tex = 0;
            output_streams[i][j].ws = std::make_tuple(i, j);
        }
    }
}

render_manager::~render_manager()
{
    for (auto& row : output_streams)
    {
        for (auto& stream : row)
            stream.buffer.release();
    }
}

wf_region render_manager::get_scheduled_damage()
{
    if (!output->destroyed)
        return output_damage->frame_damage;

    return wf_region{};
}

void render_manager::damage_whole()
{
    if (output->destroyed)
        return;

    GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
    GetTuple(vx, vy, output->workspace->get_current_workspace());

    int sw, sh;
    wlr_output_transformed_resolution(output->handle, &sw, &sh);
    output_damage->add({-vx * sw, -vy * sh, vw * sw, vh * sh});
}

void render_manager::damage_whole_idle()
{
    damage_whole();
    if (!idle_damage.is_connected())
        idle_damage.run_once([&] () { damage_whole(); });
}

void render_manager::damage(const wlr_box& box)
{
    if (!output->destroyed)
        output_damage->add(box);
}

void render_manager::damage(const wf_region& region)
{
    if (!output->destroyed)
        output_damage->add(region);
}

wlr_box render_manager::get_damage_box() const
{
    int w, h;
    wlr_output_transformed_resolution(output->handle, &w, &h);
    return {0, 0, w, h};
}

wlr_box render_manager::get_ws_box(std::tuple<int, int> ws) const
{
    GetTuple(vx, vy, ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    wlr_box box = get_damage_box();
    box.x = (vx - cx) * box.width;
    box.y = (vy - cy) * box.height;

    return box;
}

wf_framebuffer render_manager::get_target_framebuffer() const
{
    wf_framebuffer fb;
    fb.geometry = output->get_relative_geometry();
    fb.wl_transform = output->get_transform();
    fb.transform = get_output_matrix_from_transform(output->get_transform());
    fb.scale = output->handle->scale;

    fb.fb = fb.tex = 0;
    if (post_effects.size())
    {
        fb.fb = post_buffers[default_out_buffer].fb;
        fb.tex = post_buffers[default_out_buffer].tex;
    }

    fb.viewport_width = output->handle->width;
    fb.viewport_height = output->handle->height;

    return fb;
}

void render_manager::auto_redraw(bool redraw)
{
    constant_redraw += (redraw ? 1 : -1);
    if (constant_redraw > 1) /* no change, exit */
        return;
    if (constant_redraw < 0)
    {
        constant_redraw = 0;
        return;
    }

    schedule_redraw();
}

void render_manager::add_inhibit(bool add)
{
    output_inhibit += add ? 1 : -1;

    if (output_inhibit == 0)
    {
        damage_whole();
        output->emit_signal("start-rendering", nullptr);
    }
}

void render_manager::schedule_redraw()
{
    if (!idle_redraw.is_connected())
    {
        idle_redraw.run_once([&] () {
            wlr_output_schedule_frame(output->handle);
        });
    }
}

/* return damage from this frame for the given workspace, coordinates
 * relative to the workspace */
wf_region render_manager::get_ws_damage(std::tuple<int, int> ws)
{
    auto ws_box = get_ws_box(ws);
    return (frame_damage & ws_box) + wf_point{-ws_box.x, -ws_box.y};
}

void render_manager::reset_renderer()
{
    renderer = nullptr;
    damage_whole_idle();
}

void render_manager::set_renderer(render_hook_t rh)
{
    renderer = rh;
}

void render_manager::paint()
{
    /* Part 1: frame setup: query damage, etc. */
    timespec repaint_started;
    clock_gettime(CLOCK_MONOTONIC, &repaint_started);

    frame_damage.clear();
    run_effects(effects[WF_OUTPUT_EFFECT_PRE]);

    bool needs_swap;
    if (!output_damage->make_current(frame_damage, needs_swap))
        return;

    if (!needs_swap && !constant_redraw)
    {
        post_paint();
        return;
    }

    OpenGL::bind_output(output);

    /* Make sure the default buffer has enough size */
    if (post_effects.size())
    {
        OpenGL::render_begin();
        post_buffers[default_out_buffer]
            .allocate(output->handle->width, output->handle->height);
        OpenGL::render_end();
    }

    wf_region swap_damage;
    if (runtime_config.damage_debug)
    {
        swap_damage |= get_damage_box();

        OpenGL::render_begin(output->handle->width, output->handle->height, 0);
        OpenGL::clear({1, 1, 0, 1});
        OpenGL::render_end();
    }

    /* Part 2: call the renderer, which draws the scenegraph */
    if (renderer)
    {
        renderer(get_target_framebuffer());
        /* TODO: let custom renderers specify what they want to repaint... */
        swap_damage |= get_damage_box();
    } else
    {
        frame_damage &= get_damage_box();
        if (!frame_damage.empty())
        {
            swap_damage = frame_damage;
            default_renderer();
        }
    }

    /* Part 3: finalize the scene: overlay effects and sw cursors */
    run_effects(effects[WF_OUTPUT_EFFECT_OVERLAY]);

    if (post_effects.size())
        swap_damage |= get_damage_box();

    OpenGL::render_begin(get_target_framebuffer());
    wlr_output_render_software_cursors(output->handle, swap_damage.to_pixman());
    OpenGL::render_end();

    /* Part 4: postprocessing effects */
    run_post_effects();
    if (output_inhibit)
    {
        OpenGL::render_begin(output->handle->width, output->handle->height, 0);
        OpenGL::clear({0, 0, 0, 1});
        OpenGL::render_end();
    }

    /* Part 5: finalize frame: swap buffers, send frame_done, etc */
    OpenGL::unbind_output(output);
    output_damage->swap_buffers(&repaint_started, swap_damage);
    post_paint();
}

void render_manager::default_renderer()
{
    GetTuple(vx, vy, output->workspace->get_current_workspace());
    auto target_stream = &output_streams[vx][vy];
    if (current_ws_stream != target_stream)
    {
        if (current_ws_stream)
            workspace_stream_stop(current_ws_stream);

        current_ws_stream = target_stream;
        workspace_stream_start(current_ws_stream);
    } else
    {
        workspace_stream_update(current_ws_stream);
    }
}

/* Run all postprocessing effects, rendering to alternating buffers and finally
 * to the screen.
 *
 * NB: 2 buffers just aren't enough. We render to the zero buffer, and then we
 * alternately render to the second and the third. The reason: We track damage.
 * So, we need to keep the whole buffer each frame. */
void render_manager::run_post_effects()
{
    static wf_framebuffer_base default_framebuffer;
    default_framebuffer.tex = default_framebuffer.fb = 0;

    int last_buffer_idx = default_out_buffer;
    int next_buffer_idx = 1;

    post_effects.for_each([&] (auto post) -> void
    {
        /* The last postprocessing hook renders directly to the screen, others to
         * the currently free buffer */
        wf_framebuffer_base& next_buffer =
            (post == post_effects.back() ? default_framebuffer :
                post_buffers[next_buffer_idx]);

        OpenGL::render_begin();
        /* Make sure we have the correct resolution */
        next_buffer.allocate(output->handle->width, output->handle->height);
        OpenGL::render_end();

        (*post) (post_buffers[last_buffer_idx], next_buffer);

        last_buffer_idx = next_buffer_idx;
        next_buffer_idx ^= 0b11; // alternate 1 and 2
    });
}

void render_manager::post_paint()
{
    run_effects(effects[WF_OUTPUT_EFFECT_POST]);

    if (constant_redraw)
        schedule_redraw();

    auto send_frame_done =
        [=] (wayfire_view v)
        {
            if (!v->is_mapped())
                return;

            v->for_each_surface([] (wayfire_surface_t *surface, int, int)
                                {
                                    struct timespec now;
                                    clock_gettime(CLOCK_MONOTONIC, &now);
                                    surface->send_frame_done(now);
                                });
        };

    /* TODO: do this only if the view isn't fully occluded by another */
    if (renderer)
    {
        output->workspace->for_each_view(send_frame_done, WF_VISIBLE_LAYERS);
    } else
    {
        auto views = output->workspace->get_views_on_workspace(
            output->workspace->get_current_workspace(), WF_MIDDLE_LAYERS, false);

        for (auto v : views)
            send_frame_done(v);

        // send to all panels/backgrounds/etc
        output->workspace->for_each_view(send_frame_done,
            WF_BELOW_LAYERS | WF_ABOVE_LAYERS);
    }
}

void render_manager::run_effects(effect_container_t& container)
{
    container.for_each([] (auto effect)
        { (*effect)(); });
}

void render_manager::add_effect(effect_hook_t* hook, wf_output_effect_type type)
{
    effects[type].push_back(hook);
}

void render_manager::rem_effect(effect_hook_t *hook)
{
    for (int i = 0; i < WF_OUTPUT_EFFECT_TOTAL; i++)
        effects[i].remove_all(hook);
}

void render_manager::add_post(post_hook_t* hook)
{
    post_effects.push_back(hook);
    damage_whole();
}

void render_manager::rem_post(post_hook_t *hook)
{
    post_effects.remove_all(hook);
    damage_whole();
}

void render_manager::workspace_stream_start(wf_workspace_stream *stream)
{
    stream->running = true;
    stream->scale_x = stream->scale_y = 1;

    /* damage the whole workspace region, so that we get a full repaint */
    frame_damage |= get_ws_box(stream->ws);
    workspace_stream_update(stream, 1, 1);
}

void render_manager::workspace_stream_update(wf_workspace_stream *stream,
                                             float scale_x, float scale_y)
{
    auto g = output->get_relative_geometry();

    GetTuple(x, y, stream->ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int dx = g.x + (x - cx) * g.width,
        dy = g.y + (y - cy) * g.height;

    wf_region ws_damage = get_ws_damage(stream->ws);

    float current_resolution = stream->scale_x * stream->scale_y;
    float target_resolution = scale_x * scale_y;

    float scaling = std::max(current_resolution / target_resolution,
                             target_resolution / current_resolution);

    /* TODO: investigate if this still works */
    if (scaling > 2 && false)
        ws_damage |= get_damage_box();

    /* we don't have to update anything */
    if (ws_damage.empty())
        return;

    if (scale_x != stream->scale_x || scale_y != stream->scale_y)
    {
        /* FIXME: enable scaled rendering */
//        stream->scale_x = scale_x;
//        stream->scale_y = scale_y;

     //   ws_damage |= get_damage_box();
    }


    OpenGL::render_begin();
    stream->buffer.allocate(output->handle->width, output->handle->height);

    auto fb = get_target_framebuffer();
    fb.fb = (stream->buffer.fb == 0) ? fb.fb : stream->buffer.fb;
    fb.tex = (stream->buffer.tex == 0) ? fb.tex : stream->buffer.tex;

    {
        wf_stream_signal data(ws_damage, fb);
        emit_signal("workspace-stream-pre", &data);
    }

    auto views = output->workspace->get_views_on_workspace(
        stream->ws, WF_VISIBLE_LAYERS, false);

    struct damaged_surface_t
    {
        wayfire_surface_t *surface;

        int x, y; // framebuffer coords for the view
        wf_region damage;
    };

    using damaged_surface = std::unique_ptr<damaged_surface_t>;

    std::vector<damaged_surface> to_render;

    const auto schedule_render_snapshotted_view =
        [&] (wayfire_view view, int view_dx, int view_dy)
        {
            auto ds = damaged_surface(new damaged_surface_t);

            auto bbox = view->get_bounding_box() + wf_point{-view_dx, -view_dy};
            bbox = fb.damage_box_from_geometry_box(bbox);

            ds->damage = ws_damage & bbox;
            if (!ds->damage.empty())
            {
                ds->x = view_dx;
                ds->y = view_dy;
                ds->surface = view.get();

                to_render.push_back(std::move(ds));
            }
        };

    const auto schedule_render_surface =
        [&] (wayfire_surface_t *surface, int x, int y, int view_dx, int view_dy)
        {
            if (!surface->is_mapped())
                return;

            if (ws_damage.empty())
                return;

            /* make sure all coordinates are in workspace-local coords */
            x -= view_dx;
            y -= view_dy;

            auto ds = damaged_surface(new damaged_surface_t);

            auto obox = surface->get_output_geometry();
            obox.x = x;
            obox.y = y;

            obox = fb.damage_box_from_geometry_box(obox);
            ds->damage = ws_damage & obox;
            if (!ds->damage.empty())
            {
                ds->x = view_dx;
                ds->y = view_dy;
                ds->surface = surface;

                if (ds->surface->alpha >= 0.999f)
                    ds->surface->subtract_opaque(ws_damage, x, y);

                to_render.push_back(std::move(ds));
            }
        };

    /* we "move" all icons to the current output */
    if (!renderer)
    {
        if (core->input->drag_icon && core->input->drag_icon->is_mapped())
        {
            core->input->drag_icon->set_output(output);
            core->input->drag_icon->for_each_surface(
                [&] (wayfire_surface_t *surface, int x, int y)
                {
                    schedule_render_surface(surface, x, y, 0, 0);
                });
        }
    }

    auto it = views.begin();
    while (it != views.end() && !ws_damage.empty())
    {
        auto view = *it;
        int view_dx = 0, view_dy = 0;

        if (!view->is_visible())
            goto next;

        if (view->role != WF_VIEW_ROLE_SHELL_VIEW)
        {
            view_dx = dx;
            view_dy = dy;
        }

        /* We use the snapshot of a view if either condition is happening:
         * 1. The view has a transform
         * 2. The view is visible, but not mapped
         *    => it is snapshotted and kept alive by some plugin */

        /* Snapshotted views include all of their subsurfaces, so we handle them separately */
        if (view->has_transformer() || !view->is_mapped())
        {
            schedule_render_snapshotted_view(view, view_dx, view_dy);
            goto next;
        }

        /* Iterate over all subsurfaces/menus of a "regular" view */
        view->for_each_surface([&] (wayfire_surface_t *surface, int x, int y)
        { schedule_render_surface(surface, x, y, view_dx, view_dy); });

        next: ++it;
    };

    /*
     TODO; implement scale != 1
    glm::mat4 scale = glm::scale(glm::mat4(1.0), glm::vec3(scale_x, scale_y, 1));
    glm::mat4 translate = glm::translate(glm::mat4(1.0), glm::vec3(scale_x - 1, scale_y - 1, 0));
    std::swap(wayfire_view_transform::global_scale, scale);
    std::swap(wayfire_view_transform::global_translate, translate);
    */
    OpenGL::render_begin(fb);
    for (const auto& rect : ws_damage)
    {
        wlr_box damage = wlr_box_from_pixman_box(rect);
        fb.scissor(fb.framebuffer_box_from_damage_box(damage));
        OpenGL::clear(stream->background, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    OpenGL::render_end();

    for (auto& ds : wf::reverse(to_render))
    {
        fb.geometry.x = ds->x; fb.geometry.y = ds->y;
        ds->surface->render_fb(ds->damage, fb);
    }

   // std::swap(wayfire_view_transform::global_scale, scale);
   // std::swap(wayfire_view_transform::global_translate, translate);

    if (!renderer)
    {
        if (core->input->drag_icon && core->input->drag_icon->is_mapped())
            core->input->drag_icon->set_output(nullptr);
    }

    {
        wf_stream_signal data(ws_damage, fb);
        emit_signal("workspace-stream-post", &data);
    }
}

void render_manager::workspace_stream_stop(wf_workspace_stream *stream)
{
    stream->running = false;
}

/* End render_manager */
