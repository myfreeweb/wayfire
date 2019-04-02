#ifndef OUTPUT_LAYOUT_HPP
#define OUTPUT_LAYOUT_HPP

extern "C"
{
    struct wlr_backend;
    struct wlr_output_layout;
}

#include <vector>
#include <memory>
#include <functional>

#include "object.hpp"

class wayfire_output;
namespace wf
{
    /* output_layout_t is responsible for managing outputs and their attributes -
     * mode, scale, position, transform. */
    class output_layout_t : public wf_signal_provider_t
    {
        class impl;
        std::unique_ptr<impl> pimpl;

        public:
        output_layout_t(wlr_backend *backend);
        ~output_layout_t();

        /**
         * @return the underlying wlr_output_layout
         */
        wlr_output_layout *get_handle();

        /**
         * @return the output at the given coordinates, or null if no such output
         */
        wayfire_output *get_output_at(int x, int y);

        /**
         * @param lx the x coordinate of the closest point of the layout
         * @param ly the y coordinate of the closest point of the layout
         * @return the output at the given coordinates
         */
        wayfire_output *get_output_coords_at(int x, int y, int& lx, int& ly);

        /**
         * @return the number of the outputs in the output layout
         */
        size_t get_num_outputs();

        /**
         * @return a list of the outputs in the output layout
         */
        std::vector<wayfire_output*> get_outputs();

        /**
         * @return the "next" output in the layout. It is guaranteed that starting
         * with any output in the layout, and successively calling this function
         * will iterate over all outputs
         */
        wayfire_output *get_next_output(wayfire_output *output);

        /**
         * @return the wayfire_output associated with the wlr_output,
         * or null if the output isn't found
         */
        wayfire_output *find_output(wlr_output *output);
        wayfire_output *find_output(std::string name);
    };
}

#endif /* end of include guard: OUTPUT_LAYOUT_HPP */
