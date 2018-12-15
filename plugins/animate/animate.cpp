#include <output.hpp>
#include <signal-definitions.hpp>
#include <render-manager.hpp>
#include <debug.hpp>
#include <type_traits>
#include <core.hpp>
#include "system_fade.hpp"
#include "basic_animations.hpp"
#include "fire/fire.hpp"

void animation_base::init(wayfire_view, wf_option, bool) {}
bool animation_base::step() {return false;}
animation_base::~animation_base() {}

template<class animation_type, bool close_animation>
struct animation_hook;

template<class animation_type, bool close_animation>
void delete_hook_idle(void *data)
{
    auto hook = (animation_hook<animation_type, close_animation>*) data;
    delete hook;
}

template<class animation_type, bool close_animation>
struct animation_hook
{
    static_assert(std::is_base_of<animation_base, animation_type>::value,
            "animation_type must be derived from animation_base!");

    animation_base *base = nullptr;
    wayfire_view view;
    wayfire_output *output;

    effect_hook_t hook;
    signal_callback_t view_removed;

    animation_hook(wayfire_view view, wf_option duration)
    {
        this->view = view;
        output = view->get_output();

        if (close_animation)
        {
            view->inc_keep_count();
            view->take_snapshot();
        }

        view->damage();
        base = dynamic_cast<animation_base*> (new animation_type());
        base->init(view, duration, close_animation);
        base->step();
        view->damage();

        hook = [=] ()
        {
            view->damage();
            bool result = base->step();
            view->damage();

            if (!result)
                finalize();
        };

        output->render->add_effect(&hook, WF_OUTPUT_EFFECT_POST);

        view_removed = [=] (signal_data *data)
        {
            if (get_signaled_view(data) == view && !close_animation)
                finalize();
        };

        output->connect_signal("detach-view", &view_removed);

        if (!close_animation)
            output->connect_signal("view-disappeared", &view_removed);
    }

    void finalize()
    {
        output->render->rem_effect(&hook, WF_OUTPUT_EFFECT_POST);

        output->disconnect_signal("detach-view", &view_removed);
        output->disconnect_signal("view-disappeared", &view_removed);

        /* make sure we "unhide" the view */
        view->alpha = 1.0;
        delete base;

        if (close_animation)
            view->dec_keep_count();

        wl_event_loop_add_idle(core->ev_loop, delete_hook_idle<animation_type, close_animation>, this);
    }

    ~animation_hook()
    { }
};

class wayfire_animation : public wayfire_plugin_t {
    signal_callback_t map_cb, unmap_cb, wake_cb;

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

        using namespace std::placeholders;
        map_cb = std::bind(std::mem_fn(&wayfire_animation::view_mapped),
                this, _1);
        unmap_cb = std::bind(std::mem_fn(&wayfire_animation::view_unmapped),
                this, _1);

        wake_cb = [=] (signal_data *data)
        {
            new wf_system_fade(output, startup_duration);
        };

        output->connect_signal("map-view", &map_cb);
        output->connect_signal("unmap-view", &unmap_cb);

        /* TODO: the best way to do this?
        if (config->get_section("backlight")->get_int("min_brightness", 1) > -1)
            output->connect_signal("wake", &wake_cb);
            */
        output->connect_signal("start-rendering", &wake_cb);
    }

    /* TODO: enhance - add more animations */
    void view_mapped(signal_data *ddata)
    {
        auto view = get_signaled_view(ddata);
        assert(view);

        /* TODO: check if this is really needed */
        if (view->role == WF_VIEW_ROLE_SHELL_VIEW)
            return;

        if (open_animation->as_string() == "fade")
            new animation_hook<fade_animation, false>(view, duration);
        else if (open_animation->as_string() == "zoom")
            new animation_hook<zoom_animation, false>(view, duration);
        else if (open_animation->as_string() == "fire")
            new animation_hook<FireAnimation, false>(view, duration);
    }

    void view_unmapped(signal_data *data)
    {
        auto view = get_signaled_view(data);

        if (view->role == WF_VIEW_ROLE_SHELL_VIEW)
            return;

        if (close_animation->as_string() == "fade")
            new animation_hook<fade_animation, true> (view, duration);
        else if (close_animation->as_string() == "zoom")
            new animation_hook<zoom_animation, true> (view, duration);
        else if (close_animation->as_string() == "fire")
            new animation_hook<FireAnimation, true> (view, duration);
    }

    void fini()
    {
        output->disconnect_signal("map-view", &map_cb);
        output->disconnect_signal("unmap-view", &unmap_cb);
        output->disconnect_signal("start-rendering", &wake_cb);
    }
};

extern "C" {
    wayfire_plugin_t *newInstance() {
        return new wayfire_animation();
    }
}
