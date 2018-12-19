#ifndef WF_GTK_SHELL_HPP
#define WF_GTK_SHELL_HPP

#include <wayland-client.h>
#include <string>

struct wf_gtk_shell;
struct wl_resource;

wf_gtk_shell *wf_gtk_shell_create(wl_display *display);
std::string wf_gtk_shell_get_custom_app_id(wf_gtk_shell* shell,
    wl_resource *surface);

#endif /* end of include guard: WF_GTK_SHELL_HPP */
