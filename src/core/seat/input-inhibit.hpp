#ifndef INPUT_INHIBIT_HPP
#define INPUT_INHIBIT_HPP

extern "C"
{
    struct wlr_input_inhibit_manager;
}

class wayfire_output;

wlr_input_inhibit_manager* create_input_inhibit();

void inhibit_output(wayfire_output *output);
bool is_output_inhibited(wayfire_output *output);
void uninhibit_output(wayfire_output *output);

#endif /* end of include guard: INPUT_INHIBIT_HPP */
