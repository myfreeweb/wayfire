#include <map>

extern "C"
{
#include <wlr/types/wlr_input_inhibitor.h>
}

#include "signal-definitions.hpp"
#include "input-inhibit.hpp"
#include "output.hpp"
#include "plugin.hpp"
#include "core.hpp"
#include "input-manager.hpp"

static const std::string iface_name = "_input_inhibitor";
static std::map<wayfire_output*, wayfire_grab_interface> iface_map;

/* output added/removed */
static signal_callback_t on_output_changed = [] (signal_data *data)
{
    auto wo = get_signaled_output(data);

    /* if the output was inhibited, then it is being removed */
    if (is_output_inhibited(wo))
    {
        uninhibit_output(wo);
    }
    else if (core->input->exclusive_client)
    {
        inhibit_output(wo);
    }
};

wlr_input_inhibit_manager* create_input_inhibit()
{
    core->connect_signal("output-added", &on_output_changed);
    core->connect_signal("output-removed", &on_output_changed);

    return wlr_input_inhibit_manager_create(core->display);
}

void inhibit_output(wayfire_output *output)
{
    wayfire_grab_interface iface = new wayfire_grab_interface_t(output);
    iface->name = iface_name;
    iface->abilities_mask = WF_ABILITY_ALL;

    output->break_active_plugins();
    output->activate_plugin(iface);

    iface_map[output] = iface;
}

bool is_output_inhibited(wayfire_output *output)
{
    return output->is_plugin_active(iface_name);
}

void uninhibit_output(wayfire_output *output)
{
    if (!output->is_plugin_active(iface_name))
        return;

    auto iface = iface_map[output];
    assert(iface);

    iface_map.erase(output);
    output->deactivate_plugin(iface);
}
