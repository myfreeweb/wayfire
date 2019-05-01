#ifndef SYSTEM_FADE_HPP
#define SYSTEM_FADE_HPP

#include <core.hpp>
#include <output.hpp>
#include <render-manager.hpp>

#include "animate.hpp"
#include "animation.hpp"

extern "C"
{
#define static
#include <wlr/types/wlr_matrix.h>
#undef static
}

/* animates wake from suspend/startup by fading in the whole output */
class wf_system_fade
{
    wf_duration duration;

    wayfire_output *output;

    effect_hook_t damage_hook, render_hook;

    public:
        wf_system_fade(wayfire_output *out, wf_duration dur) :
            duration(dur), output(out)
        {
            damage_hook = [=] ()
            { output->render->damage_whole(); };

            render_hook = [=] ()
            { render(); };

            output->render->add_effect(&damage_hook, WF_OUTPUT_EFFECT_PRE);
            output->render->add_effect(&render_hook, WF_OUTPUT_EFFECT_OVERLAY);
            output->render->auto_redraw(true);

            duration.start(1, 0);
        }

        void render()
        {
            float color[4] = {0, 0, 0, (float)duration.progress()};

            auto geometry = output->get_relative_geometry();
            geometry = output->render->get_target_framebuffer()
                .damage_box_from_geometry_box(geometry);

            OpenGL::render_begin(output->render->get_target_framebuffer());

            float matrix[9];
            wlr_matrix_project_box(matrix, &geometry,
                                   WL_OUTPUT_TRANSFORM_NORMAL,
                                   0, output->handle->transform_matrix);

            wlr_render_quad_with_matrix(core->renderer, color, matrix);

            OpenGL::render_end();
            if (!duration.running())
                finish();
        }

        void finish()
        {
            output->render->rem_effect(&damage_hook);
            output->render->rem_effect(&render_hook);
            output->render->auto_redraw(false);

            delete this;
        }
};

#endif
