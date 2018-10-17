#include <nonstd/make_unique.hpp>

#include <opengl.hpp>
#include <view-transform.hpp>
#include <debug.hpp>
#include <core.hpp>

#include <view.hpp>
#include <output.hpp>
#include <signal-definitions.hpp>

#include <render-manager.hpp>
#include <workspace-manager.hpp>

#include <animation.hpp>

#include <algorithm>
#include <exception>
#include <set>

constexpr const char* switcher_transformer = "switcher-3d";
constexpr const char* switcher_transformer_background = "switcher-3d";

constexpr float background_dim_factor = 0.6;

struct SwitcherPaintAttribs
{
    wf_transition scale_x{1, 1}, scale_y{1, 1};
    wf_transition off_x{0, 0}, off_y{0, 0}, off_z{0, 0};
    wf_transition rotation{0, 0};
    wf_transition alpha{1, 1};
};

enum SwitcherViewPosition
{
    SWITCHER_POSITION_LEFT = 0,
    SWITCHER_POSITION_CENTER = 1,
    SWITCHER_POSITION_RIGHT = 2
};

static constexpr bool view_expired(int view_position)
{
    return view_position < SWITCHER_POSITION_LEFT ||
        view_position > SWITCHER_POSITION_RIGHT;
}

struct SwitcherView
{
    wayfire_view view;
    SwitcherPaintAttribs attribs;

    int position;

    /* Make animation start values the current progress of duration */
    void refresh_start(wf_duration duration)
    {
        attribs.off_x.start = duration.progress(attribs.off_x);
        attribs.off_y.start = duration.progress(attribs.off_y);
        attribs.off_z.start = duration.progress(attribs.off_z);

        attribs.scale_x.start = duration.progress(attribs.scale_x);
        attribs.scale_y.start = duration.progress(attribs.scale_y);

        attribs.alpha.start = duration.progress(attribs.alpha);
        attribs.rotation.start = duration.progress(attribs.rotation);
    }
};

class WayfireSwitcher : public wayfire_plugin_t
{
    wf_duration duration;
    wf_duration background_dim_duration;

    wf_option view_thumbnail_scale, touch_sensitivity;

    /* If a view comes before another in this list, it is on top of it */
    std::vector<SwitcherView> views;

    // the modifiers which were used to activate switcher
    uint32_t activating_modifiers = 0;
    key_callback next_view_binding, prev_view_binding;
    touch_gesture_callback touch_activate;

    effect_hook_t damage;
    render_hook_t switcher_renderer;

    signal_callback_t view_removed;

    bool active = false;
    public:

    void init(wayfire_config *config)
    {
        grab_interface->name = "switcher";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        switcher_renderer = [=] (uint32_t fb)
        {
            render_output(fb);
        };

        damage = [=] ()
        {
            output->render->damage(NULL);
        };

        auto section = config->get_section("switcher");

        view_thumbnail_scale = section->get_option("view_thumbnail_scale", "1.0");
        touch_sensitivity = section->get_option("touch_sensitivity", "1.0");

        auto speed = section->get_option("speed", "500");
        duration = wf_duration{speed, wf_animation::circle};
        background_dim_duration = wf_duration{speed, wf_animation::circle};

        next_view_binding = [=] (uint32_t) { handle_switch_request(-1); };
        prev_view_binding = [=] (uint32_t) { handle_switch_request(1); };

        output->add_key(section->get_option("next_view", "<super> KEY_TAB"),
            &next_view_binding);
        output->add_key(section->get_option("prev_view", "<super> <shift> KEY_TAB"),
            &prev_view_binding);

        grab_interface->callbacks.keyboard.mod = [=] (uint32_t mod, uint32_t state)
        {
            if (state == WLR_KEY_RELEASED && (mod & activating_modifiers))
                handle_done();
        };

        touch_activate = [=] (wf_touch_gesture*) {
            if (!active)
            {
                /* We set it to -1 to indicate that the user hasn't done anything yet */
                touch_total_dx = -1;
                handle_switch_request(0);
            } else {
                handle_done();
            }
        };
        output->add_gesture(new_static_option("edge-swipe down 3"), &touch_activate);

        grab_interface->callbacks.touch.down = [=] (int id, int x, int y) {
            if (id == 0) handle_touch_down(x, y);
        };

        grab_interface->callbacks.touch.up = [=] (int id) {
            if (id == 0) handle_touch_up();
        };

        grab_interface->callbacks.touch.motion = [=] (int id, int x, int y) {
            if (id == 0) handle_touch_motion(x, y);
        };

        view_removed = [=] (signal_data *data)
        {
            handle_view_removed(get_signaled_view(data));
        };
        output->connect_signal("detach-view", &view_removed);
    }

    void handle_view_removed(wayfire_view view)
    {
        log_info("handle view removed !!!!!!!!!!!");
        // not running at all, don't care
        if (!output->is_plugin_active(grab_interface->name))
            return;

        bool need_action = false;
        for (auto& sv : views)
            need_action |= (sv.view == view);

        // don't do anything if we're not using this view
        if (!need_action)
            return;

        if (active) {
            arrange();
        } else {
            cleanup_views([=] (SwitcherView& sv)
                { return sv.view == view; });
        }
    }

    void handle_switch_request(int dir)
    {
        if (get_workspace_views().empty())
            return;

        /* If we haven't grabbed, then we haven't setup anything */
        if (!output->is_plugin_active(grab_interface->name))
        {
            if (!init_switcher())
                return;
        }

        /* Maybe we're still animating the exit animation from a previous
         * switcher activation? */
        if (!active)
        {
            active = true;

            // grabs shouldn't fail if we could successfully activate plugin
            assert(grab_interface->grab());

            focus_next(dir);
            arrange();
            activating_modifiers = core->get_keyboard_modifiers();
        } else
        {
            next_view(dir);
        }
    }

    /* When switcher is done and starts animating towards end */
    void handle_done()
    {
        dearrange();
        grab_interface->ungrab();
    }

    int touch_sx, touch_total_dx = -1;
    void handle_touch_down(int x, int)
    {
        touch_sx = x;
        touch_total_dx = 0;
    }

    void handle_touch_motion(int x, int)
    {
        const float TOUCH_SENSITIVITY = 0.05 *
            touch_sensitivity->as_cached_double();

        auto og = output->get_relative_geometry();

        float dx = touch_sx - x;
        if (std::abs(dx) > TOUCH_SENSITIVITY * og.width)
        {
            touch_total_dx += touch_sx - x;
            handle_switch_request(dx > 0 ? -1 : 1);
            touch_sx = x;
        }
    }

    void handle_touch_up()
    {
        /* This means we haven't switched views, so the user wants to stop */
        if (touch_total_dx == 0)
            handle_done();
    }

    /* Sets up basic hooks needed while switcher works and/or displays animations */
    bool init_switcher()
    {
        if (!output->activate_plugin(grab_interface))
            return false;

        output->render->add_effect(&damage, WF_OUTPUT_EFFECT_PRE);
        output->render->set_renderer(switcher_renderer);
        output->render->auto_redraw(true);
        return true;
    }

    /* The reverse of init_switcher */
    void deinit_switcher()
    {
        output->deactivate_plugin(grab_interface);

        output->render->rem_effect(&damage, WF_OUTPUT_EFFECT_PRE);
        output->render->reset_renderer();
        output->render->auto_redraw(false);

        output->workspace->for_each_view([=] (wayfire_view view) {
            view->pop_transformer(switcher_transformer);
            view->pop_transformer(switcher_transformer_background);
        }, WF_ALL_LAYERS);

        views.clear();
    }

    /* offset from the left or from the right */
    float get_center_offset()
    {
        return output->get_relative_geometry().width / 3;
    }

    /* get the scale for non-focused views */
    float get_back_scale()
    {
        return 0.66;
    }

    /* offset in Z-direction for non-focused views */
    float get_z_offset()
    {
        return -1.0;
    }

    /* amount of rotation */
    float get_rotation()
    {
        return -M_PI / 6.0;
    }

    /* Move view animation target to the left
     * @param dir -1 for left, 1 for right */
    void move(SwitcherView& sv, int dir)
    {
        sv.attribs.off_x = {
            duration.progress(sv.attribs.off_x),
            sv.attribs.off_x.end + get_center_offset() * dir
        };

        sv.attribs.off_y = {
            duration.progress(sv.attribs.off_y),
            sv.attribs.off_y.end
        };

        float z_sign = 0;
        if (sv.position == SWITCHER_POSITION_CENTER)
        {
            // Move from center to either left or right, so backwards
            z_sign = 1;
        } else if (view_expired(sv.position + dir))
        {
            // Expires, don't move
            z_sign = 0;
        } else
        {
            // Not from center, doesn't expire -> comes to the center
            z_sign = -1;
        }

        sv.attribs.off_z = {
            duration.progress(sv.attribs.off_z),
            sv.attribs.off_z.end + get_z_offset() * z_sign
        };

        /* scale views that aren't in the center */
        sv.attribs.scale_x = {
            duration.progress(sv.attribs.scale_x),
            sv.attribs.scale_x.end * std::pow(get_back_scale(), z_sign)
        };

        sv.attribs.scale_y = {
            duration.progress(sv.attribs.scale_y),
            sv.attribs.scale_y.end * std::pow(get_back_scale(), z_sign)
        };

        sv.attribs.rotation = {
            duration.progress(sv.attribs.rotation),
            sv.attribs.rotation.end + get_rotation() * dir
        };

        sv.position += dir;
        sv.attribs.alpha = {
            duration.progress(sv.attribs.alpha),
            view_expired(sv.position) ? 0.3 : 1.0
        };
    }

    /* Calculate how much a view should be scaled to fit into the slots */
    float calculate_scaling_factor(const wf_geometry& bbox) const
    {
        /* Each view should not be more than this percentage of the
         * width/height of the output */
        constexpr float screen_percentage = 0.45;

        auto og = output->get_relative_geometry();

        float max_width = og.width * screen_percentage;
        float max_height = og.height * screen_percentage;

        float needed_exact = std::min(max_width / bbox.width, max_height / bbox.height);
        // don't scale down if the view is already small enough
        return std::min(needed_exact, 1.0f) * view_thumbnail_scale->as_cached_double();
    }

    /* Move untransformed view to the center */
    void arrange_center_view(SwitcherView& sv)
    {
        auto og = output->get_relative_geometry();
        auto bbox = sv.view->get_bounding_box(switcher_transformer);

        float dx = (og.width / 2 - bbox.width / 2) - bbox.x;
        float dy = bbox.y - (og.height / 2 - bbox.height / 2);

        sv.attribs.off_x = {0, dx};
        sv.attribs.off_y = {0, dy};

        float scale = calculate_scaling_factor(bbox);
        sv.attribs.scale_x = {1, scale};
        sv.attribs.scale_y = {1, scale};
    }

    /* Position the view, starting from untransformed position */
    void arrange_view(SwitcherView& sv, int position)
    {
        arrange_center_view(sv);

        if (position == SWITCHER_POSITION_CENTER)
        { /* view already centered */ }
        else {
            move(sv, position - SWITCHER_POSITION_CENTER);
        }
    }

    // returns a list of mapped views
    std::vector<wayfire_view> get_workspace_views() const
    {
        auto all_views = output->workspace->get_views_on_workspace(
            output->workspace->get_current_workspace(), WF_LAYER_WORKSPACE, true);

        decltype(all_views) mapped_views;
        for (auto view : all_views)
        {
            if (view->is_mapped())
                mapped_views.push_back(view);
        }

        return mapped_views;
    }

    /* Change the current focus to the next or the previous view */
    void focus_next(int dir)
    {
        auto ws_views = get_workspace_views();
        /* Change the focused view and rearrange views so that focused is on top */
        int size = ws_views.size();

        // calculate focus index & focus it
        int focused_view_index = (size + dir) % size;
        auto focused_view = ws_views[focused_view_index];
        output->focus_view(focused_view);
    }

    /* Create the initial arrangement on the screen
     * Also changes the focus to the next or the last view, depending on dir */
    void arrange()
    {
        // clear views in case that deinit() hasn't been run
        views.clear();

        duration.start();
        background_dim_duration.start(1, background_dim_factor);

        auto ws_views = get_workspace_views();
        log_info("restart with %lu", ws_views.size());
        for (auto v : ws_views)
            views.push_back(create_switcher_view(v));

        /* Add a copy of the unfocused view if we have just 2 */
        if (ws_views.size() == 2)
            views.push_back(create_switcher_view(ws_views.back()));

        arrange_view(views[0], SWITCHER_POSITION_CENTER);

        /* If we have just 1 view, don't do anything else */
        if (ws_views.size() > 1)
            arrange_view(views.back(), SWITCHER_POSITION_LEFT);

        for (int i = 1; i < (int)views.size() - 1; i++)
            arrange_view(views[i], SWITCHER_POSITION_RIGHT);
    }

    void dearrange()
    {
        for (auto& sv : views)
        {
            sv.attribs.off_x = {duration.progress(sv.attribs.off_x), 0};
            sv.attribs.off_y = {duration.progress(sv.attribs.off_y), 0};
            sv.attribs.off_z = {duration.progress(sv.attribs.off_z), 0};

            sv.attribs.scale_x = {duration.progress(sv.attribs.scale_x), 1.0};
            sv.attribs.scale_y = {duration.progress(sv.attribs.scale_y), 1.0};

            sv.attribs.rotation = {duration.progress(sv.attribs.rotation), 0};
            sv.attribs.alpha    = {duration.progress(sv.attribs.alpha), 1.0};
        }

        background_dim_duration.start(background_dim_duration.progress(), 1);
        duration.start();
        active = false;
    }

    std::vector<wayfire_view> get_background_views() const
    {
        return output->workspace->get_views_on_workspace(
            output->workspace->get_current_workspace(), WF_BELOW_LAYERS, false);
    }

    void dim_background(float dim)
    {
        for (auto view : get_background_views())
        {
            if (dim == 1.0)
            {
                view->pop_transformer(switcher_transformer_background);
            } else
            {
                if (!view->get_transformer(switcher_transformer_background))
                {
                    view->add_transformer(nonstd::make_unique<wf_3D_view> (view),
                        switcher_transformer_background);
                }

                auto tr = dynamic_cast<wf_3D_view*> (
                    view->get_transformer(switcher_transformer_background).get());
                tr->color[0] = tr->color[1] = tr->color[2] = dim;
            }
        }
    }

    SwitcherView create_switcher_view(wayfire_view view)
    {
        /* we add a view transform if there isn't any.
         *
         * Note that a view might be visible on more than 1 place, so damage
         * tracking doesn't work reliably. To circumvent this, we simply damage
         * the whole output */
        if (!view->get_transformer(switcher_transformer))
        {
            view->add_transformer(nonstd::make_unique<wf_3D_view> (view),
                switcher_transformer);
        }

        return SwitcherView{view, {}, SWITCHER_POSITION_CENTER};
    }

    void render_view(const SwitcherView& sv)
    {
        auto transform = dynamic_cast<wf_3D_view*> (
            sv.view->get_transformer(switcher_transformer).get());
        assert(transform);

        transform->translation = glm::translate(
            glm::mat4(1.0), {
                duration.progress(sv.attribs.off_x),
                duration.progress(sv.attribs.off_y),
                duration.progress(sv.attribs.off_z)});

        transform->scaling = glm::scale(
            glm::mat4(1.0), {
                duration.progress(sv.attribs.scale_x),
                duration.progress(sv.attribs.scale_y),
                1.0});

        transform->rotation = glm::rotate(
            glm::mat4(1.0), (float)duration.progress(sv.attribs.rotation),
            {0.0, 1.0, 0.0});

        transform->color[3] = duration.progress(sv.attribs.alpha);
        sv.view->render_fb(NULL, output->render->get_target_framebuffer());
    }

    void render_output(uint32_t fb)
    {
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb));
        OpenGL::use_device_viewport();

        wlr_renderer_scissor(core->renderer, NULL);

        GL_CALL(glClearColor(0.0, 0.0, 0.0, 1.0));
        GL_CALL(glClear(GL_COLOR_BUFFER_BIT));

        dim_background(background_dim_duration.progress());
        for (auto view : get_background_views())
            view->render_fb(NULL, output->render->get_target_framebuffer());

        /* Render in the reverse order because we don't use depth testing */
        auto it = views.rbegin();
        while (it != views.rend())
        {
            render_view(*it);
            ++it;
        }

        if (!duration.running())
        {
            cleanup_expired();

            if (!active)
                deinit_switcher();
        }

        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    /* delete all views matching the given criteria, skipping the first "start" views */
    void cleanup_views(std::function<bool(SwitcherView&)> criteria)
    {
        auto it = views.begin();
        while(it != views.end())
        {
            if (criteria(*it)) {
                it = views.erase(it);
            } else {
                ++it;
            }
        }
    }

    /* Removes all expired views from the list */
    void cleanup_expired()
    {
        cleanup_views([=] (SwitcherView& sv)
            { return view_expired(sv.position); });
    }

    /* sort views according to their Z-order */
    void rebuild_view_list()
    {
        std::stable_sort(views.begin(), views.end(),
            [] (const SwitcherView& a, const SwitcherView& b)
            {
                enum category {
                    FOCUSED = 0,
                    UNFOCUSED = 1,
                    EXPIRED = 2
                };

                auto view_category = [] (const SwitcherView& sv)
                {
                    if (sv.position == SWITCHER_POSITION_CENTER)
                        return FOCUSED;
                    if (view_expired(sv.position))
                        return EXPIRED;
                    return UNFOCUSED;
                };

                category aCat = view_category(a), bCat = view_category(b);
                if (aCat == bCat) {
                    return a.position < b.position;
                } else {
                    return aCat < bCat;
                }
            });
    }

    void next_view(int dir)
    {
        cleanup_expired();

        if (count_different_active_views() <= 1)
            return;

        /* Count of views in the left/right slots */
        int count_right = 0;
        int count_left = 0;

        /* Move the topmost view from the center and the left/right group,
         * depending on the direction*/
        int to_move = (1 << SWITCHER_POSITION_CENTER) | (1 << (1 - dir));
        for (auto& sv : views)
        {
            if (!view_expired(sv.position) && ((1 << sv.position) & to_move))
            {
                to_move ^= (1 << sv.position); // only the topmost one
                move(sv, dir);
            } else if (!view_expired(sv.position))
            {
                /* Make sure animations start from where we are now */
                sv.refresh_start(duration);
            }

            count_left += (sv.position == SWITCHER_POSITION_LEFT);
            count_right += (sv.position == SWITCHER_POSITION_RIGHT);
        }

        /* Create a new view on the missing slot, but if both are missing,
         * show just the centered view */
        if (bool(count_left) ^ bool(count_right))
        {
            const int empty_slot = 1 - dir;
            fill_emtpy_slot(empty_slot);
        }

        rebuild_view_list();
        output->focus_view(views.front().view);
        duration.start();
    }

    int count_different_active_views()
    {
        std::set<wayfire_view> active_views;
        for (auto& sv : views)
            active_views.insert(sv.view);

        return active_views.size();
    }

    /* Move the last view in the given slot so that it becomes invalid */
    wayfire_view invalidate_last_in_slot(int slot)
    {
        for (int i = views.size() - 1; i >= 0; i--)
        {
            if (views[i].position == slot)
            {
                move(views[i], slot - 1);
                return views[i].view;
            }
        }

        return nullptr;
    }

    /* Returns the non-focused view in the case where there is only 1 view */
    wayfire_view get_unfocused_view()
    {
        for (auto& sv : views)
        {
            if (!view_expired(sv.position) &&
                sv.position != SWITCHER_POSITION_CENTER)
            {
                return sv.view;
            }
        }

        return nullptr;
    }

    void fill_emtpy_slot(const int empty_slot)
    {
        const int full_slot = 2 - empty_slot;

        /* We have an empty slot. We invalidate the bottom-most view in the
         * opposite slot, and create a new view with the same content to
         * fill in the empty slot */
        auto view_to_create = invalidate_last_in_slot(full_slot);

        /* special case: we have just 2 views
         * in this case, the "new" view should not be the same as the
         * invalidated view(because this view is focused now), but the
         * one which isn't focused */
        if (count_different_active_views() == 2)
            view_to_create = get_unfocused_view();
        assert(view_to_create);

        auto sv = create_switcher_view(view_to_create);
        arrange_view(sv, empty_slot);

        /* directly show it on the target position */
        sv.refresh_start({new_static_option("0")});
        sv.attribs.alpha = {0, 1};

        views.push_back(sv);
    }
};

extern "C"
{
    wayfire_plugin_t* newInstance()
    {
        return new WayfireSwitcher();
    }
}
