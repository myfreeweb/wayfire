#include <cassert>
#include <algorithm>
#include <libinput.h>
#include <nonstd/make_unique.hpp>

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

    log_info("handle new input: %s, default mapping: %s", dev->name, dev->output_name);
    input_devices.push_back(nonstd::make_unique<wf_input_device> (dev));

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
        keyboards.push_back(nonstd::make_unique<wf_keyboard> (dev, core->config));

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->attach_device(dev);
        pointer_count++;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count++;
        if (!our_touch)
            our_touch = std::unique_ptr<wf_touch> (new wf_touch(cursor->cursor));

        our_touch->add_device(dev);
    }

    auto section = core->config->get_section(nonull(dev->name));
    auto mapped_output = section->get_option("output",
        nonull(dev->output_name))->as_string();

    auto wo = core->get_output(mapped_output);
    if (wo)
        wlr_cursor_map_input_to_output(cursor->cursor, dev, wo->handle);

    update_capabilities();
}

void input_manager::handle_input_destroyed(wlr_input_device *dev)
{
    log_info("remove input: %s", dev->name);

    auto it = std::remove_if(input_devices.begin(), input_devices.end(),
        [=] (const std::unique_ptr<wf_input_device>& idev) { return idev->device == dev; });
    input_devices.erase(it, input_devices.end());

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        auto it = std::remove_if(keyboards.begin(), keyboards.end(),
                                 [=] (const std::unique_ptr<wf_keyboard>& kbd) { return kbd->device == dev; });

        keyboards.erase(it, keyboards.end());
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        cursor->detach_device(dev);
        pointer_count--;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
        touch_count--;

    update_capabilities();
}

input_manager::input_manager()
{
    input_device_created.notify = handle_new_input_cb;
    seat = wlr_seat_create(core->display, "default");

    wl_signal_add(&core->backend->events.new_input,
                  &input_device_created);

    surface_map_state_changed = [=] (signal_data *data)
    {
        update_cursor_position(get_current_time(), false);

        if (our_touch)
        {
            for (auto f : our_touch->gesture_recognizer.current)
                handle_touch_motion(get_current_time(), f.first, f.second.sx, f.second.sy);
        }
    };

    config_updated = [=] (signal_data *)
    {
        for (auto& dev : input_devices)
            dev->update_options();
        for (auto& kbd : keyboards)
            kbd->reload_input_options();
    };

    core->connect_signal("reload-config", &config_updated);

    /*

    session_listener.notify = session_signal_handler;
    wl_signal_add(&core->ec->session_signal, &session_listener);
    */
}

input_manager::~input_manager()
{
    core->disconnect_signal("reload-config", &config_updated);
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

    set_keyboard_focus(NULL, seat);
    update_cursor_focus(nullptr, 0, 0);
    core->set_cursor("default");
    return true;
}

static void idle_update_cursor(void *data)
{
    auto input = (input_manager*) data;
    input->update_cursor_position(get_current_time(), false);
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
}

bool input_manager::input_grabbed()
{
    return active_grab || !session_active;
}

int input_manager::get_num_inputs()
{
    int count = pressed_buttons.size();
    if (our_touch)
        count += our_touch->gesture_recognizer.current.size();
    return count;
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

wf_binding* input_manager::new_binding(wf_binding_type type, wf_option value,
    wayfire_output *output, void *callback)
{
    auto binding = nonstd::make_unique<wf_binding>();

    assert(value && output && callback);

    binding->type = type;
    binding->value = value;
    binding->output = output;
    binding->call.raw = callback;

    auto raw = binding.get();
    bindings[type].push_back(std::move(binding));

    return raw;
}

void input_manager::rem_binding(binding_criteria criteria)
{
    for(auto& category : bindings)
    {
        auto& container = category.second;
        auto it = container.begin();
        while (it != container.end())
        {
            if (criteria((*it).get())) {
                it = container.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void input_manager::rem_binding(wf_binding *binding)
{
    rem_binding([=] (wf_binding *ptr) { return binding == ptr; });
}

void input_manager::rem_binding(void *callback)
{
    rem_binding([=] (wf_binding* ptr) {return ptr->call.raw == callback; });
}

void input_manager::free_output_bindings(wayfire_output *output)
{
    rem_binding([=] (wf_binding* binding) {
        return binding->output == output;
    });
}
