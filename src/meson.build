wayfire_sources = ['main.cpp',
                   'util.cpp',

                   'core/opengl.cpp',
                   'core/plugin.cpp',
                   'core/core.cpp',
                   'core/wm.cpp',

                   'core/seat/input-inhibit.cpp',
                   'core/seat/input-manager.cpp',
                   'core/seat/keyboard.cpp',
                   'core/seat/cursor.cpp',
                   'core/seat/touch.cpp',
                   'core/seat/seat.cpp',

                   'view/surface.cpp',
                   'view/view.cpp',
                   'view/view-util.cpp',
                   'view/xdg-shell-v6.cpp',
                   'view/xdg-shell.cpp',
                   'view/xwayland.cpp',
                   'view/layer-shell.cpp',
                   'view/view-3d.cpp',
                   'view/compositor-view.cpp',

                   'output/plugin-loader.cpp',
                   'output/output.cpp',
                   'output/render-manager.cpp',
                   'output/wayfire-shell.cpp',
                   'output/gtk-shell.cpp']

wayfire_dependencies = [wayland_server, wlroots, xkbcommon, libinput,
                       pixman, drm, egl, libevdev, glesv2, glm, wf_protos,
                       wfconfig]

if conf_data.get('BUILD_WITH_IMAGEIO')
    wayfire_sources += ['core/img.cpp']
    wayfire_dependencies += [jpeg, png]
endif

executable('wayfire', wayfire_sources,
        dependencies: wayfire_dependencies,
        include_directories: [wayfire_conf_inc, wayfire_api_inc],
        link_args: '-ldl',
        install: true)
