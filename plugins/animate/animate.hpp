#ifndef ANIMATE_H_
#define ANIMATE_H_

#include <view.hpp>
#include <animation.hpp>

enum wf_animation_type
{
    ANIMATION_TYPE_MAP,
    ANIMATION_TYPE_UNMAP,
    ANIMATION_TYPE_MINIMIZE,
    ANIMATION_TYPE_RESTORE
};

class animation_base
{
    public:
    virtual void init(wayfire_view view, wf_option duration, wf_animation_type type);
    virtual bool step(); /* return true if continue, false otherwise */
    virtual ~animation_base();
};

#endif
