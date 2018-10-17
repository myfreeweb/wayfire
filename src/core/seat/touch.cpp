#include <cmath>

#include "debug.hpp"
#include "touch.hpp"
#include "input-manager.hpp"
#include "core.hpp"
#include "output.hpp"
#include "workspace-manager.hpp"
#include "compositor-surface.hpp"
#include "../../view/priv-view.hpp"

constexpr static int MIN_FINGERS = 3;
constexpr static int MIN_SWIPE_DISTANCE = 100;
constexpr static float MIN_PINCH_DISTANCE = 70;
constexpr static int EDGE_SWIPE_THRESHOLD = 50;

void wf_gesture_recognizer::reset_gesture()
{
    gesture_emitted = false;

    int cx = 0, cy = 0;
    for (auto f : current)
    {
        cx += f.second.sx;
        cy += f.second.sy;
    }

    cx /= current.size();
    cy /= current.size();

    start_sum_dist = 0;
    for (auto &f : current)
    {
        start_sum_dist += std::sqrt((cx - f.second.sx) * (cx - f.second.sx)
                                  + (cy - f.second.sy) * (cy - f.second.sy));

        f.second.ix = f.second.sx;
        f.second.iy = f.second.sy;
    }
}

void wf_gesture_recognizer::start_new_gesture(int reason_id, int time)
{
    in_gesture = true;
    reset_gesture();

    for (auto &f : current)
    {
        if (f.first != reason_id)
            core->input->handle_touch_up(time, f.first);
    }
}

void wf_gesture_recognizer::stop_gesture()
{
    in_gesture = gesture_emitted = false;
}

void wf_gesture_recognizer::continue_gesture(int id, int sx, int sy)
{
    if (gesture_emitted)
        return;

    /* first case - consider swipe, we go through each
     * of the directions and check whether such swipe has occured */

    bool is_left_swipe = true, is_right_swipe = true,
         is_up_swipe = true, is_down_swipe = true;

    for (auto f : current) {
        int dx = f.second.sx - f.second.ix;
        int dy = f.second.sy - f.second.iy;

        if (-MIN_SWIPE_DISTANCE < dx)
            is_left_swipe = false;
        if (dx < MIN_SWIPE_DISTANCE)
            is_right_swipe = false;

        if (-MIN_SWIPE_DISTANCE < dy)
            is_up_swipe = false;
        if (dy < MIN_SWIPE_DISTANCE)
            is_down_swipe = false;
    }

    uint32_t swipe_dir = 0;
    if (is_left_swipe)
        swipe_dir |= GESTURE_DIRECTION_LEFT;
    if (is_right_swipe)
        swipe_dir |= GESTURE_DIRECTION_RIGHT;
    if (is_up_swipe)
        swipe_dir |= GESTURE_DIRECTION_UP;
    if (is_down_swipe)
        swipe_dir |= GESTURE_DIRECTION_DOWN;

    if (swipe_dir)
    {
        wayfire_touch_gesture gesture;
        gesture.type = GESTURE_SWIPE;
        gesture.finger_count = current.size();
        gesture.direction = swipe_dir;

        bool bottom_edge = false, upper_edge = false,
             left_edge = false, right_edge = false;

        auto og = core->get_active_output()->get_full_geometry();

        for (auto f : current)
        {
            bottom_edge |= (f.second.iy >= og.y + og.height - EDGE_SWIPE_THRESHOLD);
            upper_edge  |= (f.second.iy <= og.y + EDGE_SWIPE_THRESHOLD);
            left_edge   |= (f.second.ix <= og.x + EDGE_SWIPE_THRESHOLD);
            right_edge  |= (f.second.ix >= og.x + og.width - EDGE_SWIPE_THRESHOLD);
        }

        uint32_t edge_swipe_dir = 0;
        if (bottom_edge)
            edge_swipe_dir |= GESTURE_DIRECTION_UP;
        if (upper_edge)
            edge_swipe_dir |= GESTURE_DIRECTION_DOWN;
        if (left_edge)
            edge_swipe_dir |= GESTURE_DIRECTION_RIGHT;
        if (right_edge)
            edge_swipe_dir |= GESTURE_DIRECTION_LEFT;

        if ((edge_swipe_dir & swipe_dir) == swipe_dir)
            gesture.type = GESTURE_EDGE_SWIPE;

        core->input->handle_gesture(gesture);
        gesture_emitted = true;
        return;
    }

    /* second case - this has been a pinch.
     * We calculate the central point of the fingers (cx, cy),
     * then we measure the average distance to the center. If it
     * is bigger/smaller above/below some threshold, then we emit the gesture */
    int cx = 0, cy = 0;
    for (auto f : current) {
        cx += f.second.sx;
        cy += f.second.sy;
    }

    cx /= current.size();
    cy /= current.size();

    int sum_dist = 0;
    for (auto f : current) {
        sum_dist += std::sqrt((cx - f.second.sx) * (cx - f.second.sx)
                              + (cy - f.second.sy) * (cy - f.second.sy));
    }

    bool inward_pinch  = (start_sum_dist - sum_dist >= MIN_PINCH_DISTANCE);
    bool outward_pinch = (start_sum_dist - sum_dist <= -MIN_PINCH_DISTANCE);

    if (inward_pinch || outward_pinch) {
        wayfire_touch_gesture gesture;
        gesture.type = GESTURE_PINCH;
        gesture.finger_count = current.size();
        gesture.direction =
            (inward_pinch ? GESTURE_DIRECTION_IN : GESTURE_DIRECTION_OUT);

        core->input->handle_gesture(gesture);
        gesture_emitted = true;
    }
}

void wf_gesture_recognizer::update_touch(int32_t time, int id, int sx, int sy)
{
    current[id].sx = sx;
    current[id].sy = sy;

    if (in_gesture)
    {
        continue_gesture(id, sx, sy);
    } else
    {
        core->input->handle_touch_motion(time, id, sx, sy);
    }
}

void wf_gesture_recognizer::register_touch(int time, int id, int sx, int sy)
{
    current[id] = {id, sx, sy, sx, sy};
    if (in_gesture)
        reset_gesture();

    if (current.size() >= MIN_FINGERS && !in_gesture)
        start_new_gesture(id, time);

    if (!in_gesture)
        core->input->handle_touch_down(time, id, sx, sy);
}

void wf_gesture_recognizer::unregister_touch(int32_t time, int32_t id)
{
    /* shouldn't happen, except possibly in nested(wayland/x11) backend */
    if (!current.count(id))
        return;

    current.erase(id);
    if (in_gesture)
    {
        if (current.size() < MIN_FINGERS)
            stop_gesture();
        else
            reset_gesture();
    }
    else
    {
        core->input->handle_touch_up(time, id);
    }
}

static void handle_touch_down(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_down*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(core->input->cursor,
                                         ev->device, ev->x, ev->y, &lx, &ly);

    touch->gesture_recognizer.register_touch(ev->time_msec, ev->touch_id, lx, ly);
    wlr_idle_notify_activity(core->protocols.idle, core->get_current_seat());
}

static void handle_touch_up(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_up*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    touch->gesture_recognizer.unregister_touch(ev->time_msec, ev->touch_id);
    wlr_idle_notify_activity(core->protocols.idle, core->get_current_seat());
}

static void handle_touch_motion(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_motion*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(core->input->cursor,
                                         ev->device, ev->x, ev->y, &lx, &ly);
    touch->gesture_recognizer.update_touch(ev->time_msec, ev->touch_id, lx, ly);
    wlr_idle_notify_activity(core->protocols.idle, core->get_current_seat());
}

wf_touch::wf_touch(wlr_cursor *cursor)
{
    down.notify   = handle_touch_down;
    up.notify     = handle_touch_up;
    motion.notify = handle_touch_motion;

    wl_signal_add(&cursor->events.touch_up, &up);
    wl_signal_add(&cursor->events.touch_down, &down);
    wl_signal_add(&cursor->events.touch_motion, &motion);

    this->cursor = cursor;
}

void wf_touch::add_device(wlr_input_device *device)
{
    device->data = this;
    wlr_cursor_attach_input_device(cursor, device);
}

/* input_manager touch functions */
void input_manager::set_touch_focus(wayfire_surface_t *surface, uint32_t time, int id, int x, int y)
{
    bool focus_compositor_surface = wf_compositor_surface_from_surface(surface);
    bool had_focus = wlr_seat_touch_get_point(seat, id);

    wlr_surface *next_focus = NULL;
    if (surface && !focus_compositor_surface)
        next_focus = surface->surface;

    // create a new touch point, we have a valid new focus
    if (!had_focus && next_focus)
        wlr_seat_touch_notify_down(seat, next_focus, time, id, x, y);
    if (had_focus && !next_focus)
        wlr_seat_touch_notify_up(seat, time, id);

    if (next_focus)
        wlr_seat_touch_point_focus(seat, next_focus, time, id, x, y);

    /* Manage the touch_focus, we take only the first finger for that */
    if (id == 0)
    {
        auto compositor_surface = wf_compositor_surface_from_surface(touch_focus);
        if (compositor_surface)
            compositor_surface->on_touch_up();

        compositor_surface = wf_compositor_surface_from_surface(surface);
        if (compositor_surface)
            compositor_surface->on_touch_down(x, y);

        touch_focus = surface;
    }
}

void input_manager::handle_touch_down(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    auto wo = core->get_output_at(x, y);
    core->focus_output(wo);

    auto og = wo->get_full_geometry();
    int ox = x - og.x;
    int oy = y - og.y;

    if (id == 0)
        core->input->check_touch_bindings(ox, oy);


    if (active_grab)
    {
        if (active_grab->callbacks.touch.down)
            active_grab->callbacks.touch.down(id, ox, oy);
        return;
    }

    int lx, ly;
    auto focus = input_surface_at(x, y, lx, ly);
    set_touch_focus(focus, time, id, lx, ly);
    update_drag_icons();
}

void input_manager::handle_touch_up(uint32_t time, int32_t id)
{
    if (active_grab)
    {
        if (active_grab->callbacks.touch.up)
            active_grab->callbacks.touch.up(id);

        return;
    }

    set_touch_focus(nullptr, time, id, 0, 0);
}

void input_manager::handle_touch_motion(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    if (active_grab)
    {
        auto wo = core->get_output_at(x, y);
        auto og = wo->get_full_geometry();
        if (active_grab->callbacks.touch.motion)
            active_grab->callbacks.touch.motion(id, x - og.x, y - og.y);

        return;
    }

    int lx, ly;
    auto surface = input_surface_at(x, y, lx, ly);
    set_touch_focus(surface, time, id, lx, ly);
    wlr_seat_touch_notify_motion(seat, time, id, lx, ly);

    update_drag_icons();

    auto compositor_surface = wf_compositor_surface_from_surface(touch_focus);
    if (id == 0 && compositor_surface)
        compositor_surface->on_touch_motion(lx, ly);
}

void input_manager::check_touch_bindings(int x, int y)
{
    uint32_t mods = get_modifiers();
    std::vector<touch_callback*> calls;
    for (auto listener : touch_listeners)
    {
        if (listener.second.mod == mods &&
                listener.second.output == core->get_active_output())
        {
            calls.push_back(listener.second.call);
        }
    }

    for (auto call : calls)
        (*call)(x, y);
}

void input_manager::handle_gesture(wayfire_touch_gesture g)
{
    for (const auto& listener : gesture_listeners)
    {
        const auto& direction = listener.second.gesture.direction;
        if (listener.second.gesture.type == g.type &&
            listener.second.gesture.finger_count == g.finger_count &&
            (direction == 0 || direction == g.direction) &&
            core->get_active_output() == listener.second.output)
        {
            (*listener.second.call)(&g);
        }
    }
}

