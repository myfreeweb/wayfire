#ifndef PRIV_VIEW_HPP
#define PRIV_VIEW_HPP

#include <view.hpp>
#include <decorator.hpp>
#include <assert.h>
#include <nonstd/make_unique.hpp>

// for emit_map_*()
#include <compositor-view.hpp>
#include <compositor-surface.hpp>

wayfire_surface_t* wf_surface_from_void(void *handle);
wayfire_view_t* wf_view_from_void(void *handle);

void init_xdg_shell_v6();
void init_xdg_shell();
void init_xwayland();
void init_layer_shell();

struct wlr_seat;
void xwayland_set_seat(wlr_seat *seat);
std::string xwayland_get_display();

void init_desktop_apis();

#endif /* end of include guard: PRIV_VIEW_HPP */

