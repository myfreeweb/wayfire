#ifndef FIRE_ANIMATION_HPP
#define FIRE_ANIMATION_HPP

#include <view-transform.hpp>
#include <render-manager.hpp>
#include <memory>

#include "../animate.hpp"

class FireTransformer;
class FireAnimation : public animation_base
{
    std::string name; // the name of the transformer in the view's table
    wayfire_view view;
    nonstd::observer_ptr<FireTransformer> transformer;
    effect_hook_t hook, damage;
    wf_duration duration;

    public:

    static wf_option fire_particles, fire_particle_size;

    ~FireAnimation();
    virtual void init(wayfire_view view, wf_option duration, wf_animation_type type);
    virtual bool step(); /* return true if continue, false otherwise */
};

#endif /* end of include guard: FIRE_ANIMATION_HPP */
