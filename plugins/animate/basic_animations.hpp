#include "animate.hpp"
#include <debug.hpp>
#include <plugin.hpp>
#include <opengl.hpp>
#include <view-transform.hpp>
#include <nonstd/make_unique.hpp>
#include <output.hpp>

class fade_animation : public animation_base
{
    wayfire_view view;

    float start = 0, end = 1;
    wf_duration duration;
    std::string name;

    public:

    void init(wayfire_view view, wf_option dur, wf_animation_type type)
    {
        this->view = view;
        duration = wf_duration(dur);
        duration.start();

        if (type == ANIMATION_TYPE_UNMAP)
            std::swap(start, end);

        name = "animation-fade-" + std::to_string(type);
        view->add_transformer(nonstd::make_unique<wf_2D_view> (view), name);
    }

    bool step()
    {
        auto transform = dynamic_cast<wf_2D_view*> (view->get_transformer(name).get());
        transform->alpha = duration.progress(start, end);
        return duration.running();
    }

    ~fade_animation()
    {
        view->alpha = 1.0f;
        view->pop_transformer(name);
    }
};

class zoom_animation : public animation_base
{
    wayfire_view view;
    wf_2D_view *our_transform = nullptr;

    float alpha_start = 0, alpha_end = 1;
    float zoom_start = 1./3, zoom_end = 1;
    wf_duration duration;

    public:

    void init(wayfire_view view, wf_option dur, wf_animation_type type)
    {
        this->view = view;
        duration = wf_duration(dur);
        duration.start();

        if (type == ANIMATION_TYPE_UNMAP)
        {
            std::swap(alpha_start, alpha_end);
            std::swap(zoom_start, zoom_end);
        }

        our_transform = new wf_2D_view(view);
        view->add_transformer(std::unique_ptr<wf_2D_view> (our_transform));
    }

    bool step()
    {
        float c = duration.progress(zoom_start, zoom_end);
        our_transform->alpha = duration.progress(alpha_start, alpha_end);
        our_transform->scale_x = c;
        our_transform->scale_y = c;

        return duration.running();
    }

    ~zoom_animation()
    {
        view->pop_transformer(nonstd::make_observer(our_transform));
        view->alpha = 1.0;
    }
};
