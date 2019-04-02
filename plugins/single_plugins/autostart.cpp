#include <plugin.hpp>
#include <core.hpp>

class wayfire_autostart_core_data : public wf_custom_data_t { };

class wayfire_autostart : public wayfire_plugin_t
{
    public:
    void init(wayfire_config *config)
    {
        /* Run only once, at startup */
        if (core->has_data<wayfire_autostart_core_data> ())
            return;

        auto section = config->get_section("autostart");
        for (const auto& command : section->options)
            core->run(command->as_string().c_str());

        core->get_data_safe<wayfire_autostart_core_data>();
    }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_autostart();
    }
}
