#include <output.hpp>
#include <signal-definitions.hpp>
#include <render-manager.hpp>
#include <debug.hpp>
#include <type_traits>
#include <core.hpp>
#include "system_fade.hpp"
#include "basic_animations.hpp"
#include "fire/fire.hpp"

void animation_base::init(wayfire_view, wf_option, wf_animation_type) {}
bool animation_base::step() {return false;}
animation_base::~animation_base() {}

template<class animation_type>
struct animation_hook;

template<class animation_type>
void delete_hook_idle(void *data)
{
    auto hook = (animation_hook<animation_type>*) data;
    delete hook;
}

template<class animation_type>
struct animation_hook
{
    static_assert(std::is_base_of<animation_base, animation_type>::value,
            "animation_type must be derived from animation_base!");

    wf_animation_type type;
    animation_base *base = nullptr;
    wayfire_view view;
    wayfire_output *output;

    effect_hook_t update_animation_hook = [=] ()
    {
        view->damage();
        bool result = base->step();
        view->damage();

        if (!result)
            finalize(false);
    };

    signal_callback_t view_removed = [=] (signal_data *data)
    {
        if (get_signaled_view(data) == view)
            finalize(true);
    };

    animation_hook(wayfire_view view, wf_option duration, wf_animation_type type)
    {
        this->type = type;
        this->view = view;
        this->output = view->get_output();

        if (type == ANIMATION_TYPE_UNMAP)
        {
            view->inc_keep_count();
            view->take_snapshot();
        }

        base = dynamic_cast<animation_base*> (new animation_type());
        base->init(view, duration, type);

        output->render->add_effect(&update_animation_hook, WF_OUTPUT_EFFECT_PRE);

        output->connect_signal("detach-view", &view_removed);
        /* We don't want to listen for view-disappeared signal,
         * because it will be emitted right after the unmap signal */
        if (type != ANIMATION_TYPE_UNMAP)
            output->connect_signal("view-disappeared", &view_removed);
        output->connect_signal("view-minimize-request", &view_removed);
    }

    void finalize(bool forced)
    {
        output->render->rem_effect(&update_animation_hook, WF_OUTPUT_EFFECT_PRE);

        output->disconnect_signal("detach-view", &view_removed);
        output->disconnect_signal("view-disappeared", &view_removed);
        output->disconnect_signal("view-minimize-request", &view_removed);

        delete base;
        if (type == ANIMATION_TYPE_UNMAP)
            view->dec_keep_count();

        if (type == ANIMATION_TYPE_MINIMIZE && !forced)
            view->set_minimized(true);

        wl_event_loop_add_idle(core->ev_loop, delete_hook_idle<animation_type>, this);
    }

    ~animation_hook()
    { }
};

class wayfire_animation : public wayfire_plugin_t
{
    wf_option open_animation, close_animation;
    wf_option duration, startup_duration;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "animate";
        grab_interface->abilities_mask = WF_ABILITY_CUSTOM_RENDERING;

        auto section     = config->get_section("animate");
        open_animation   = section->get_option("open_animation", "fade");
        close_animation  = section->get_option("close_animation", "fade");
        duration         = section->get_option("duration", "300");
        startup_duration = section->get_option("startup_duration", "600");

        FireAnimation::fire_particles =
            section->get_option("fire_particles", "2000");
        FireAnimation::fire_particle_size =
            section->get_option("fire_particle_size", "16");

        output->connect_signal("map-view", &on_view_mapped);
        output->connect_signal("unmap-view", &on_view_unmapped);
        output->connect_signal("start-rendering", &on_render_start);
        output->connect_signal("view-minimize-request", &on_minimize_request);
    }

    /* TODO: enhance - add more animations */
    signal_callback_t on_view_mapped = [=] (signal_data *ddata) -> void
    {
        auto view = get_signaled_view(ddata);
        assert(view);

        /* TODO: check if this is really needed */
        if (view->role == WF_VIEW_ROLE_SHELL_VIEW)
            return;

        if (open_animation->as_string() == "fade")
            new animation_hook<fade_animation>(view, duration, ANIMATION_TYPE_MAP);
        else if (open_animation->as_string() == "zoom")
            new animation_hook<zoom_animation>(view, duration, ANIMATION_TYPE_MAP);
        else if (open_animation->as_string() == "fire")
            new animation_hook<FireAnimation>(view, duration, ANIMATION_TYPE_MAP);
    };

    signal_callback_t on_view_unmapped = [=] (signal_data *data) -> void
    {
        auto view = get_signaled_view(data);

        if (view->role == WF_VIEW_ROLE_SHELL_VIEW)
            return;

        if (close_animation->as_string() == "fade")
            new animation_hook<fade_animation> (view, duration, ANIMATION_TYPE_UNMAP);
        else if (close_animation->as_string() == "zoom")
            new animation_hook<zoom_animation> (view, duration, ANIMATION_TYPE_UNMAP);
        else if (close_animation->as_string() == "fire")
            new animation_hook<FireAnimation> (view, duration, ANIMATION_TYPE_UNMAP);
    };

    signal_callback_t on_minimize_request = [=] (signal_data *data) -> void
    {
        auto ev = static_cast<view_minimize_request_signal*> (data);
        if (ev->state) {
            ev->carried_out = true;
            new animation_hook<zoom_animation> (ev->view, duration, ANIMATION_TYPE_MINIMIZE);
        } else {
            new animation_hook<zoom_animation> (ev->view, duration, ANIMATION_TYPE_RESTORE);
        }
    };

    signal_callback_t on_render_start = [=] (signal_data *data) -> void
    {
        new wf_system_fade(output, startup_duration);
    };

    void fini()
    {
        output->disconnect_signal("map-view", &on_view_mapped);
        output->disconnect_signal("unmap-view", &on_view_unmapped);
        output->disconnect_signal("start-rendering", &on_render_start);
        output->disconnect_signal("view-minimize-request", &on_minimize_request);
    }
};

extern "C" {
    wayfire_plugin_t *newInstance() {
        return new wayfire_animation();
    }
}
