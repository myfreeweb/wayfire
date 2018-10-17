#include <cassert>
#include <algorithm>
#include <libinput.h>

#include <iostream>

extern "C"
{
#include <wlr/types/wlr_seat.h>
}

#include "input-inhibit.hpp"
#include "signal-definitions.hpp"
#include "core.hpp"
#include "touch.hpp"
#include "keyboard.hpp"
#include "cursor.hpp"
#include "input-manager.hpp"
#include "workspace-manager.hpp"
#include "debug.hpp"

bool input_manager::is_touch_enabled()
{
    return touch_count > 0;
}

void input_manager::update_capabilities()
{
    uint32_t cap = 0;
    if (pointer_count)
        cap |= WL_SEAT_CAPABILITY_POINTER;
    if (keyboards.size())
        cap |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (touch_count)
        cap |= WL_SEAT_CAPABILITY_TOUCH;

    wlr_seat_set_capabilities(seat, cap);
}

void handle_new_input_cb(wl_listener*, void *data)
{
    auto dev = static_cast<wlr_input_device*> (data);
    assert(dev);
    core->input->handle_new_input(dev);
}

void input_manager::handle_new_input(wlr_input_device *dev)
{
    if (!cursor)
        create_seat();

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
        keyboards.push_back(std::unique_ptr<wf_keyboard> (new wf_keyboard(dev, core->config)));

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        wlr_cursor_attach_input_device(cursor, dev);
        pointer_count++;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count++;
        if (!our_touch)
            our_touch = std::unique_ptr<wf_touch> (new wf_touch(cursor));

        log_info("has touch devi with output %s", dev->output_name);

        our_touch->add_device(dev);
    }

    if (wlr_input_device_is_libinput(dev))
        configure_input_device(wlr_libinput_get_device_handle(dev));

    auto section = core->config->get_section(nonull(dev->name));
    auto mapped_output = section->get_option("output", nonull(dev->output_name))->raw_value;

    auto wo = core->get_output(mapped_output);
    if (wo)
        wlr_cursor_map_input_to_output(cursor, dev, wo->handle);

    update_capabilities();
}

void input_manager::handle_input_destroyed(wlr_input_device *dev)
{
    log_info("add new input: %s", dev->name);
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        auto it = std::remove_if(keyboards.begin(), keyboards.end(),
                                 [=] (const std::unique_ptr<wf_keyboard>& kbd) { return kbd->device == dev; });

        keyboards.erase(it, keyboards.end());
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        wlr_cursor_detach_input_device(cursor, dev);
        pointer_count--;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
        touch_count--;

    update_capabilities();
}

static uint64_t get_input_time()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

input_manager::input_manager()
{
    input_device_created.notify = handle_new_input_cb;
    seat = wlr_seat_create(core->display, "default");

    wl_signal_add(&core->backend->events.new_input,
                  &input_device_created);

    surface_map_state_changed = [=] (signal_data *data)
    {
        update_cursor_position(get_input_time(), false);

        if (our_touch)
        {
            for (auto f : our_touch->gesture_recognizer.current)
                handle_touch_motion(get_input_time(), f.first, f.second.sx, f.second.sy);
        }
    };

    /*

    session_listener.notify = session_signal_handler;
    wl_signal_add(&core->ec->session_signal, &session_listener);
    */
}

uint32_t input_manager::get_modifiers()
{
    uint32_t mods = 0;
    auto keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard)
        mods = wlr_keyboard_get_modifiers(keyboard);

    return mods;
}

bool input_manager::grab_input(wayfire_grab_interface iface)
{
    if (!iface || !iface->grabbed || !session_active)
        return false;

    assert(!active_grab); // cannot have two active input grabs!

    if (our_touch)
        for (const auto& f : our_touch->gesture_recognizer.current)
            handle_touch_up(0, f.first);

    active_grab = iface;

    auto kbd = wlr_seat_get_keyboard(seat);
    auto mods = kbd->modifiers;
    mods.depressed = 0;
    wlr_seat_keyboard_send_modifiers(seat, &mods);

    iface->output->set_keyboard_focus(NULL, seat);
    update_cursor_focus(nullptr, 0, 0);
    core->set_cursor("default");
    return true;
}

static void idle_update_cursor(void *data)
{
    auto input = (input_manager*) data;
    // TODO: use CLOCK_MONOTONIC instead of last_cursor_event_msec
    input->update_cursor_position(input->last_cursor_event_msec, false);
}

void input_manager::ungrab_input()
{
    if (active_grab)
        active_grab->output->set_active_view(active_grab->output->get_active_view());
    active_grab = nullptr;

    /* We must update cursor focus, however, if we update "too soon", the current
     * pointer event (button press/release, maybe something else) will be sent to
     * the client, which shouldn't happen (at the time of the event, there was
     * still an active input grab) */
    wl_event_loop_add_idle(core->ev_loop, idle_update_cursor, this);

    /*
    if (is_touch_enabled())
        gr->end_grab();
        */
}

bool input_manager::input_grabbed()
{
    return active_grab || !session_active;
}

void input_manager::toggle_session()
{

    session_active ^= 1;
    if (!session_active)
    {
        if (active_grab)
        {
            auto grab = active_grab;
            ungrab_input();
            active_grab = grab;
        }
    } else
    {
        if (active_grab)
        {
            auto grab = active_grab;
            active_grab = nullptr;
            grab_input(grab);
        }
    }

}

bool input_manager::can_focus_surface(wayfire_surface_t *surface)
{
    if (exclusive_client && surface->get_client() != exclusive_client)
        return false;

    return true;
}

wayfire_surface_t* input_manager::input_surface_at(int x, int y,
    int& lx, int& ly)
{
    auto output = core->get_output_at(x, y);
    assert(output);

    auto og = output->get_full_geometry();
    x -= og.x;
    y -= og.y;

    wayfire_surface_t *new_focus = nullptr;
    output->workspace->for_each_view(
        [&] (wayfire_view view)
        {
            if (new_focus) return; // we already found a focus surface

            if (can_focus_surface(view.get())) // make sure focusing this surface isn't disabled
                new_focus = view->map_input_coordinates(x, y, lx, ly);
        },
        WF_ALL_LAYERS);

    return new_focus;
}

void input_manager::set_exclusive_focus(wl_client *client)
{
    exclusive_client = client;

    core->for_each_output([&client] (wayfire_output *output)
    {
        if (client)
            inhibit_output(output);
        else
            uninhibit_output(output);
    });
}

/* add/remove bindings */

static int _last_id = 0;
#define id_deleter(type) \
\
void input_manager::rem_ ##type (int id) \
{ \
    auto it = type ## _bindings.find(id); \
    if (it != type ## _bindings.end()) \
    { \
        delete it->second; \
        type ## _bindings.erase(it); \
    } \
}

#define callback_deleter(type) \
void input_manager::rem_ ##type (type ## _callback *cb) \
{ \
    auto it = type ## _bindings.begin(); \
\
    while(it != type ## _bindings.end()) \
    { \
        if (it->second->call == cb) \
        { \
            delete it->second; \
            it = type ## _bindings.erase(it); \
        } else \
            ++it; \
    } \
}

int input_manager::add_key(wf_option option, key_callback *call, wayfire_output *output)
{
    auto kcd = new key_callback_data;
    kcd->call = call;
    kcd->output = output;
    kcd->key = option;
    kcd->id = ++_last_id;

    key_bindings[_last_id] = kcd;
    return _last_id;
}

id_deleter(key);
callback_deleter(key);

int input_manager::add_axis(wf_option option, axis_callback *call, wayfire_output *output)
{
    auto acd = new axis_callback_data;
    acd->call = call;
    acd->output = output;
    acd->modifier = option;
    acd->id = ++_last_id;

    axis_bindings[_last_id] = acd;
    return _last_id;
}

id_deleter(axis);
callback_deleter(axis);

int input_manager::add_button(wf_option option, button_callback *call, wayfire_output *output)
{
    auto bcd = new button_callback_data;
    bcd->call = call;
    bcd->output = output;
    bcd->button = option;
    bcd->id = ++_last_id;

    button_bindings[_last_id] = bcd;
    return _last_id;
}

id_deleter(button);
callback_deleter(button);

int input_manager::add_touch(uint32_t mods, touch_callback* call, wayfire_output *output)
{
    int sz = 0;
    if (!touch_listeners.empty())
        sz = (--touch_listeners.end())->first + 1;

    touch_listeners[sz] = {mods, call, output};
    return sz;
}

void input_manager::rem_touch(int id)
{
    touch_listeners.erase(id);
}

void input_manager::rem_touch(touch_callback *tc)
{
    std::vector<int> ids;
    for (const auto& x : touch_listeners)
        if (x.second.call == tc)
            ids.push_back(x.first);

    for (auto x : ids)
        rem_touch(x);
}

int input_manager::add_gesture(wf_option option, touch_gesture_callback *callback,
    wayfire_output *output)
{
    gesture_listeners[gesture_id] = {option, callback, output};
    gesture_id++;
    return gesture_id - 1;
}

void input_manager::rem_gesture(int id)
{
    gesture_listeners.erase(id);
}

void input_manager::rem_gesture(touch_gesture_callback *cb)
{
    std::vector<int> ids;
    for (const auto& x : gesture_listeners)
        if (x.second.call == cb)
            ids.push_back(x.first);

    for (auto x : ids)
        rem_gesture(x);
}

void input_manager::free_output_bindings(wayfire_output *output)
{
    std::vector<int> bindings;
    for (auto kcd : key_bindings)
        if (kcd.second->output == output)
            bindings.push_back(kcd.second->id);

    for (auto x : bindings)
        rem_key(x);

    bindings.clear();
    for (auto bcd : button_bindings)
        if (bcd.second->output == output)
            bindings.push_back(bcd.second->id);

    for (auto x : bindings)
        rem_button(x);

    std::vector<int> ids;
    for (const auto& x : touch_listeners)
        if (x.second.output == output)
            ids.push_back(x.first);
    for (auto x : ids)
        rem_touch(x);

    ids.clear();
    for (const auto& x : gesture_listeners)
        if (x.second.output == output)
            ids.push_back(x.first);
    for (auto x : ids)
        rem_gesture(x);
}


