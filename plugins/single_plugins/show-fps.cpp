#include <output.hpp>
#include <render-manager.hpp>

class WfShowFps : public wayfire_plugin_t
{
    public:
        effect_hook_t damage, overlay;
        void init(wayfire_config *config)
        {
            damage = [=] () {
                // ideally adapt this (pass a wlr_box instead of NULL)
                // so that you damage only the FPS not the whole screen
                output->render->damage(NULL);
            };
            output->render->add_effect(&damage, WF_OUTPUT_EFFECT_PRE);

            overlay = [=] () {
                // render the FPS in any way you want
            };
            output->render->add_effect(&overlay, WF_OUTPUT_EFFECT_OVERLAY);
        }

        void fini()
        {
            output->render->rem_effect(&damage, WF_OUTPUT_EFFECT_PRE);
            output->render->rem_effect(&overlay, WF_OUTPUT_EFFECT_OVERLAY);
        }
};

extern "C"
{
    wayfire_plugin_t* newInstance()
    {
        return new WfShowFps();
    }
}
