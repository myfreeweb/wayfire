#include <view.hpp>
#include <debug.hpp>
#include <output.hpp>
#include <core.hpp>
#include <workspace-manager.hpp>
#include <render-manager.hpp>
#include <signal-definitions.hpp>
#include <opengl.hpp>
#include <list>
#include <algorithm>

struct wf_default_workspace_implementation : public wf_workspace_implementation
{
    bool view_movable (wayfire_view view)  { return true; }
    bool view_resizable(wayfire_view view) { return true; }
    virtual ~wf_default_workspace_implementation() {}
};

using wf_layer_container = std::list<wayfire_view>;

class viewport_manager : public workspace_manager
{
    struct custom_viewport_layer_data_t : public wf_custom_data_t
    {
        uint32_t layer = 0;
    };

    uint32_t& _get_view_layer(wayfire_view view)
    {
        return view->get_data_safe<custom_viewport_layer_data_t>()->layer;
    }

    private:
        int vwidth, vheight, vx, vy;
        wayfire_output *output;
        wf_geometry output_geometry;

        wf_layer_container layers[WF_TOTAL_LAYERS];

        inline int layer_index_from_mask(uint32_t layer_mask) const
        { return __builtin_ctz(layer_mask); }

        signal_callback_t adjust_fullscreen_layer, view_detached,
                          view_changed_viewport, output_geometry_changed;

        wf_geometry current_workarea;
        std::vector<anchored_area*> anchors;

        std::vector<std::vector<wf_workspace_implementation*>> implementation;
        wf_default_workspace_implementation default_implementation;

        void check_lower_panel_layer(int base);
        bool sent_autohide = false;

        void remove_from_layer(wayfire_view view, uint32_t layer);

    public:
        void init(wayfire_output *output);
        virtual ~viewport_manager();

        bool view_visible_on(wayfire_view, std::tuple<int, int>);
        bool view_visible_on(wayfire_view, std::tuple<int, int>, bool use_bbox);
        void move_to_workspace(wayfire_view view, std::tuple<int, int>);

        std::vector<wayfire_view>
            get_views_on_workspace(std::tuple<int, int> ws, uint32_t layer_mask, bool wm_only);
        void for_each_view(view_callback_proc_t call, uint32_t layers_mask);
        void for_each_view_reverse(view_callback_proc_t call, uint32_t layers_mask);

        /* Directly move the view to the given layer */
        void _add_view_to_layer(wayfire_view view, uint32_t layer);

        /* The difference to the previous function is that this one will adjust
         * the fullscreen layer if necessary */
        void add_view_to_layer(wayfire_view view, uint32_t layer);


        uint32_t get_view_layer(wayfire_view view);

        wf_workspace_implementation* get_implementation(std::tuple<int, int>);
        bool set_implementation(std::tuple<int, int>, wf_workspace_implementation*, bool override = false);

        void set_workspace(std::tuple<int, int>);

        std::tuple<int, int> get_current_workspace();
        std::tuple<int, int> get_workspace_grid_size();

        wf_geometry calculate_anchored_geometry(const anchored_area& area);

        void update_output_geometry();

        void add_reserved_area(anchored_area *area);
        void reflow_reserved_areas();
        void remove_reserved_area(anchored_area *area);

        wf_geometry get_workarea();
};

/* Start viewport_manager */
void viewport_manager::init(wayfire_output *o)
{
    output = o;
    vx = vy = 0;

    current_workarea = output->get_relative_geometry();
    output_geometry = output->get_relative_geometry();

    vwidth = core->vwidth;
    vheight = core->vheight;
    implementation.resize(vwidth, std::vector<wf_workspace_implementation*>
            (vheight, &default_implementation));

    adjust_fullscreen_layer = [=] (signal_data *data)
    {
        auto conv = static_cast<view_maximized_signal*> (data);
        assert(conv);

        if (conv->state != conv->view->fullscreen)
            check_lower_panel_layer(conv->state ? 1 : -1);
        else
            check_lower_panel_layer(0);
    };

    view_detached = [=] (signal_data *data)
    {
        auto view = get_signaled_view(data);
        assert(view);

        check_lower_panel_layer(view->fullscreen ? -1 : 0);
    };

    view_changed_viewport = [=] (signal_data *data)
    {
        check_lower_panel_layer(0);
    };

    output_geometry_changed = [=] (signal_data *data)
    {
        update_output_geometry();
        reflow_reserved_areas();
    };

    o->connect_signal("view-fullscreen-request", &adjust_fullscreen_layer);
    o->connect_signal("attach-view", &view_detached);
    o->connect_signal("detach-view", &view_detached);
    o->connect_signal("view-change-viewport", &view_changed_viewport);
    o->connect_signal("output-configuration-changed", &output_geometry_changed);
}

viewport_manager::~viewport_manager()
{
}

void viewport_manager::remove_from_layer(wayfire_view view, uint32_t layer)
{
    auto& layer_container = layers[layer];

    auto it = std::remove(layer_container.begin(), layer_container.end(), view);
    layer_container.erase(it, layer_container.end());
}

void viewport_manager::_add_view_to_layer(wayfire_view view, uint32_t layer)
{
    /* make sure it is a valid layer */
    assert(layer == 0 || layer == uint32_t(-1) ||
        (__builtin_popcount(layer) == 1 && layer < (1 << WF_TOTAL_LAYERS)));

    view->damage();
    auto& current_layer = _get_view_layer(view);

    /* Just remove from layer */
    if (layer == 0)
    {
        if (current_layer)
            remove_from_layer(view, layer_index_from_mask(current_layer));

        current_layer = 0;
        return;
    }

    if (layer == (uint32_t)-1 && current_layer == 0)
    {
        log_error ("trying to bring_to_front a view without a layer!");
        return;
    }

    /* -1 means bring to front */
    if (layer == (uint32_t)-1)
        layer = current_layer;

    if (current_layer)
        remove_from_layer(view, layer_index_from_mask(current_layer));

    auto& layer_container = layers[layer_index_from_mask(layer)];
    layer_container.push_front(view);
    current_layer = layer;
    view->damage();
}

void viewport_manager::add_view_to_layer(wayfire_view view, uint32_t layer)
{
    uint32_t real_layer = layer;
    if (real_layer == (uint32_t)-1)
        real_layer = get_view_layer(view);

    bool view_in_middle_layers = (real_layer & WF_MIDDLE_LAYERS);
    /* To focus view is fullscreen, go straight for the fullscreen layer */
    if (view->fullscreen && view_in_middle_layers)
        return _add_view_to_layer(view, WF_LAYER_FULLSCREEN);

    /* If we bring to front a view which isn't fullscreen, lower the fs layer */
    if (!view->fullscreen && view_in_middle_layers)
    {
        auto views = get_views_on_workspace(get_current_workspace(),
            WF_LAYER_FULLSCREEN, true);

        for (auto& v : wf::reverse(views))
            _add_view_to_layer(v, WF_LAYER_WORKSPACE);

        return _add_view_to_layer(view, layer);
    }

    /* Maybe we remove a view and the one below it is fullscreen */
    if (layer == 0 && (get_view_layer(view) & WF_MIDDLE_LAYERS))
    {
        /* Remove it from the list */
        _add_view_to_layer(view, layer);

        auto views = get_views_on_workspace(get_current_workspace(),
            WF_LAYER_WORKSPACE, true);

        if (views.size() && views[0]->fullscreen)
            _add_view_to_layer(views[0], WF_LAYER_FULLSCREEN);

        return;
    }

    /* Special cases which may need adjusting the fullscreen layer are over.
     * Simply change the view layer */
    _add_view_to_layer(view, layer);
}

uint32_t viewport_manager::get_view_layer(wayfire_view view)
{
    return _get_view_layer(view);
}

bool viewport_manager::view_visible_on(wayfire_view view, std::tuple<int, int> vp)
{
    return view_visible_on(view, vp, true);
}

bool viewport_manager::view_visible_on(wayfire_view view, std::tuple<int, int> vp, bool use_bbox)
{
    GetTuple(tx, ty, vp);

    auto g = output->get_relative_geometry();
    if (view->role != WF_VIEW_ROLE_SHELL_VIEW)
    {
        g.x += (tx - vx) * g.width;
        g.y += (ty - vy) * g.height;
    }

    if (view->has_transformer() & use_bbox)
        return view->intersects_region(g);
    else
        return g & view->get_wm_geometry();
}

void viewport_manager::move_to_workspace(wayfire_view view, std::tuple<int, int> ws)
{
    if (view->get_output() != output)
    {
        log_error("Cannot ensure view visibility for a view from a different output!");
        return;
    }

    GetTuple(wx, wy, ws);
    GetTuple(cx, cy, get_current_workspace());

    auto box = view->get_wm_geometry();
    auto visible = output->get_relative_geometry();
    visible.x += (wx - cx) * visible.width;
    visible.y += (wy - cy) * visible.height;

    if (!(box & visible))
    {
        /* center of the view */
        int cx = box.x + box.width / 2;
        int cy = box.y + box.height / 2;

        int width = visible.width, height = visible.height;
        /* compute center coordinates when moved to the current workspace */
        int local_cx = (cx % width + width) % width;
        int local_cy = (cy % height + height) % height;

        /* finally, calculate center coordinates in the target workspace */
        int target_cx = local_cx + visible.x;
        int target_cy = local_cy + visible.y;

        view->move(box.x + target_cx - cx, box.y + target_cy - cy);
    }
}

void viewport_manager::for_each_view(view_callback_proc_t call, uint32_t layers_mask)
{
    std::vector<wayfire_view> views;

    for (int i = WF_TOTAL_LAYERS - 1; i >= 0; i--)
    {
        if ((1 << i) & layers_mask)
            for (auto v : layers[i])
                views.push_back(v);
    }

    for (auto v : views)
        call(v);
}

void viewport_manager::for_each_view_reverse(view_callback_proc_t call, uint32_t layers_mask)
{
    std::vector<wayfire_view> views;

    for (int i = WF_TOTAL_LAYERS - 1; i >= 0; i--)
    {
        if ((1 << i) & layers_mask)
            for (auto v : layers[i])
                views.push_back(v);
    }

    for (auto& view : wf::reverse(views))
        call(view);
}

wf_workspace_implementation* viewport_manager::get_implementation(std::tuple<int, int> vt)
{
    GetTuple(x, y, vt);
    return implementation[x][y];
}

bool viewport_manager::set_implementation(std::tuple<int, int> vt, wf_workspace_implementation* impl, bool override)
{
    GetTuple(x, y, vt);
    bool replace = override || implementation[x][y] == nullptr;

    if (replace)
        implementation[x][y] = impl;

    return replace;
}

std::tuple<int, int> viewport_manager::get_current_workspace()
{
    return std::make_tuple(vx, vy);
}

std::tuple<int, int> viewport_manager::get_workspace_grid_size()
{
    return std::make_tuple(vwidth, vheight);
}

void viewport_manager::set_workspace(std::tuple<int, int> nPos)
{
    GetTuple(nx, ny, nPos);
    if(nx >= vwidth || ny >= vheight || nx < 0 || ny < 0)
    {
        log_error("Attempt to set invalid workspace: %d,%d,"
            " workspace grid size is %dx%d", nx, ny, vwidth, vheight);
        return;
    }

    if (nx == vx && ny == vy)
    {
        auto views = get_views_on_workspace(std::make_tuple(vx, vy), WF_MIDDLE_LAYERS, true);
        if (views.size() >= 1)
            output->focus_view(views[0]);
        return;
    }

    GetTuple(sw, sh, output->get_screen_size());
    auto dx = (vx - nx) * sw;
    auto dy = (vy - ny) * sh;

    for_each_view([=] (wayfire_view v) {
        v->move(v->get_wm_geometry().x + dx, v->get_wm_geometry().y + dy);
    }, WF_MIDDLE_LAYERS);

    output->render->schedule_redraw();

    change_viewport_signal data;
    data.old_viewport = std::make_tuple(vx, vy);
    data.new_viewport = std::make_tuple(nx, ny);

    vx = nx;
    vy = ny;
    output->emit_signal("viewport-changed", &data);

    output->focus_view(nullptr);
    /* we iterate through views on current viewport from bottom to top
     * that way we ensure that they will be focused befor all others */
    auto views = get_views_on_workspace(std::make_tuple(vx, vy), WF_MIDDLE_LAYERS, true);
    for (auto& view : wf::reverse(views))
    {
        if (view->is_mapped() && !view->destroyed)
            output->focus_view(view);
    }

    check_lower_panel_layer(0);
}

std::vector<wayfire_view>
viewport_manager::get_views_on_workspace(std::tuple<int, int> vp,
                                         uint32_t layers_mask, bool wm_only)
{

    std::vector<wayfire_view> views;

    for (int i = WF_TOTAL_LAYERS - 1; i >= 0; i--)
    {
        if ((1 << i) & layers_mask)
        {
            for (auto v : layers[i])
            {
                if (view_visible_on(v, vp, !wm_only))
                    views.push_back(v);
            }
        }
    }

    return views;
}

wf_geometry viewport_manager::get_workarea()
{
    return current_workarea;
}

wf_geometry viewport_manager::calculate_anchored_geometry(const anchored_area& area)
{
    auto wa = get_workarea();
    wf_geometry target;

    if (area.edge <= WORKSPACE_ANCHORED_EDGE_BOTTOM)
    {
        target.width = wa.width;
        target.height = area.real_size;
    } else
    {
        target.height = wa.height;
        target.width = area.real_size;
    }

    target.x = wa.x;
    target.y = wa.y;

    if (area.edge == WORKSPACE_ANCHORED_EDGE_RIGHT)
        target.x = wa.x + wa.width - target.width;

    if (area.edge == WORKSPACE_ANCHORED_EDGE_BOTTOM)
        target.y = wa.y + wa.height - target.height;

    return target;
}

void viewport_manager::add_reserved_area(anchored_area *area)
{
    anchors.push_back(area);
}

void viewport_manager::remove_reserved_area(anchored_area *area)
{
    auto it = std::remove(anchors.begin(), anchors.end(), area);
    anchors.erase(it, anchors.end());
}

void viewport_manager::reflow_reserved_areas()
{
    auto old_workarea = current_workarea;

    current_workarea = output->get_relative_geometry();
    for (auto a : anchors)
    {
        auto anchor_area = calculate_anchored_geometry(*a);

        if (a->reflowed)
            a->reflowed(anchor_area, current_workarea);

        switch(a->edge)
        {
            case WORKSPACE_ANCHORED_EDGE_TOP:
                current_workarea.y += a->reserved_size;
                // fallthrough
            case WORKSPACE_ANCHORED_EDGE_BOTTOM:
                current_workarea.height -= a->reserved_size;
                break;

            case WORKSPACE_ANCHORED_EDGE_LEFT:
                current_workarea.x += a->reserved_size;
                // fallthrough
            case WORKSPACE_ANCHORED_EDGE_RIGHT:
                current_workarea.width -= a->reserved_size;
                break;
        }
    }

    reserved_workarea_signal data;
    data.old_workarea = old_workarea;
    data.new_workarea = current_workarea;

    if (data.old_workarea != data.new_workarea)
        output->emit_signal("reserved-workarea", &data);
}

void viewport_manager::update_output_geometry()
{
    auto old_w = output_geometry.width, old_h = output_geometry.height;
    GetTuple(new_w, new_h, output->get_screen_size());

    for_each_view([=] (wayfire_view view)
    {
        if (!view->is_mapped())
            return;

       auto wm = view->get_wm_geometry();
            float px = 1. * wm.x / old_w;
            float py = 1. * wm.y / old_h;
            float pw = 1. * wm.width / old_w;
            float ph = 1. * wm.height / old_h;

            view->set_geometry({int(px * new_w), int(py * new_h),
			    int(pw * new_w), int(ph * new_h)});
    }, WF_MIDDLE_LAYERS);
    output_geometry = output->get_relative_geometry();
}

void viewport_manager::check_lower_panel_layer(int base)
{
    auto views = get_views_on_workspace(get_current_workspace(), WF_MIDDLE_LAYERS, true);

    int cnt_fullscreen = base;
    for (auto v : views)
        cnt_fullscreen += (v->fullscreen ? 1 : 0);

    log_info("send autohide %d", base);
    if (cnt_fullscreen)
    {
        if (!sent_autohide)
        {
            sent_autohide = 1;
            output->emit_signal("autohide-panels", reinterpret_cast<signal_data*> (1));
        }
    } else
    {
        if (sent_autohide)
        {
            sent_autohide = 0;
            output->emit_signal("autohide-panels", reinterpret_cast<signal_data*> (0));
        }
    }
}

class viewport_impl_plugin : public wayfire_plugin_t {

    void init(wayfire_config *config)
    {
        auto vp = new viewport_manager();
        vp->init(output);
        output->workspace = vp;
    }

    void fini()
    {
        delete output->workspace;
        output->workspace = NULL;
    }

    bool is_unloadable() { return false; }
    bool is_internal() { return true; }
};

extern "C" {
    wayfire_plugin_t *newInstance()
    {
        return new viewport_impl_plugin();
    }
}

