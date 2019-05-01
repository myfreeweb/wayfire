#ifndef WORKSPACE_MANAGER_HPP
#define WORKSPACE_MANAGER_HPP

#include <functional>
#include <vector>
#include <view.hpp>

using view_callback_proc_t = std::function<void(wayfire_view)>;

struct wf_workspace_implementation
{
    virtual bool view_movable(wayfire_view view) = 0;
    virtual bool view_resizable(wayfire_view view) = 0;
    virtual ~wf_workspace_implementation() {}
};

enum wf_layer
{
    WF_LAYER_BACKGROUND = (1 << 0),
    WF_LAYER_BOTTOM     = (1 << 1),
    WF_LAYER_WORKSPACE  = (1 << 2),
    WF_LAYER_XWAYLAND   = (1 << 3),
    WF_LAYER_TOP        = (1 << 4),
    WF_LAYER_FULLSCREEN = (1 << 5),
    WF_LAYER_LOCK       = (1 << 6),
    WF_LAYER_MINIMIZED  = (1 << 7)
};

#define WF_TOTAL_LAYERS 8

#define WF_WM_LAYERS     (WF_LAYER_WORKSPACE  | WF_LAYER_FULLSCREEN)
#define WF_MIDDLE_LAYERS (WF_WM_LAYERS        | WF_LAYER_XWAYLAND)
#define WF_ABOVE_LAYERS  (WF_LAYER_TOP        | WF_LAYER_LOCK)
#define WF_BELOW_LAYERS  (WF_LAYER_BACKGROUND | WF_LAYER_BOTTOM)

#define WF_VISIBLE_LAYERS (WF_MIDDLE_LAYERS | WF_ABOVE_LAYERS | WF_BELOW_LAYERS)
#define WF_ALL_LAYERS     (WF_VISIBLE_LAYERS | WF_LAYER_MINIMIZED)

/* return all layers not below layer, ie. layers above it + the layer itself */
uint32_t wf_all_layers_not_below(uint32_t layer);

/* workspace manager controls various workspace-related functions.
 * Currently it is implemented as a plugin, see workspace_viewport_implementation plugin */
class workspace_manager
{
    public:
        /* return if the view is visible on the given workspace */
        virtual bool view_visible_on(wayfire_view view, std::tuple<int, int>) = 0;

        /* adjust view attributes so that its geometry is visible on the
         * given workspace */
        virtual void move_to_workspace(wayfire_view view, std::tuple<int, int> ws) = 0;

        /* returns a list of the views in the given layers on the given workspace
         * @param wm_only - if set, this will return only the views whose WM geometry is inside
         * the workspace. See view.hpp for a distinction between wm, output and boundingbox geometry */
        virtual std::vector<wayfire_view>
            get_views_on_workspace(std::tuple<int, int> ws, uint32_t layer_mask, bool wm_only) = 0;
        virtual void for_each_view(view_callback_proc_t call, uint32_t layers_mask) = 0;
        virtual void for_each_view_reverse(view_callback_proc_t call, uint32_t layers_mask) = 0;

        /* TODO: split this api? */

        /* if layer_mask == 0, then we remove the view from its layer,
         * if layer_mask == -1, then the view will be moved to the top of its layer */
        virtual void add_view_to_layer(wayfire_view view, uint32_t layer) = 0;

        virtual uint32_t get_view_layer(wayfire_view view) = 0;

        /* return the active wf_workspace_implementation for the given workpsace */
        virtual wf_workspace_implementation* get_implementation(std::tuple<int, int>) = 0;

        /* returns true if implementation of workspace has been successfully installed.
         * @param override - override current implementation if it is existing.
         * it must be guaranteed that if override is set, then the functions returns true */
        virtual bool set_implementation(std::tuple<int, int>, wf_workspace_implementation *, bool override = false) = 0;

        virtual void set_workspace(std::tuple<int, int>) = 0;
        virtual std::tuple<int, int> get_current_workspace() = 0;
        virtual std::tuple<int, int> get_workspace_grid_size() = 0;

        enum anchored_edge
        {
            WORKSPACE_ANCHORED_EDGE_TOP = 0,
            WORKSPACE_ANCHORED_EDGE_BOTTOM = 1,
            WORKSPACE_ANCHORED_EDGE_LEFT = 2,
            WORKSPACE_ANCHORED_EDGE_RIGHT = 3
        };

        struct anchored_area
        {
            anchored_edge edge;
            /* amount of space to reserve */
            int reserved_size;

            /* desired size, to be given later in the reflowed callback */
            int real_size;

            /* called when the anchored area geometry was changed.
             * First geometry is the anchored geometry, the second one the
             * workarea available when the anchored area was considered */
            std::function<void(wf_geometry, wf_geometry)> reflowed;
        };

        virtual wf_geometry calculate_anchored_geometry(const anchored_area& area) = 0;

        /* Add a reserved area. The actual recalculation must be manually
         * triggered by calling reflow_reserved_areas() */
        virtual void add_reserved_area(anchored_area *area) = 0;

        /* Remove a reserved area. The actual recalculation must be manually
         * triggered by calling reflow_reserved_areas() */
        virtual void remove_reserved_area(anchored_area *area) = 0;

        /* Recalculate reserved area for each anchored area */
        virtual void reflow_reserved_areas() = 0;

        /* returns the available area for views, it is basically
         * the output geometry minus the area reserved for panels */
        virtual wf_geometry get_workarea() = 0;

        virtual ~workspace_manager();
};

#endif /* end of include guard: WORKSPACE_MANAGER_HPP */
