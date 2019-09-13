#pragma once

#include <string>
#include <wayfire/util.hpp>
#include <wayfire/surface.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/nonstd/noncopyable.hpp>

#include <cairo.h>

namespace wf
{
namespace decor
{
class decoration_theme_t;

enum button_type_t
{
    BUTTON_CLOSE,
};

class button_t : public noncopyable_t
{
  public:
    button_t(const decoration_theme_t& theme);

    /**
     * Set the type of the button. This will affect the displayed icon and
     * potentially other appearance like colors.
     */
    void set_button_type(button_type_t type);

    /** @return The type of the button */
    button_type_t get_button_type() const;

    /**
     * Set the button hover state.
     * Affects appearance.
     */
    void set_hover(bool is_hovered);

    /**
     * Set whether the button is pressed or not.
     * Affects appearance.
     */
    void set_pressed(bool is_pressed);

    /**
     * @return Whether the button needs to be repainted
     */
    bool needs_repaint();

    /**
     * Render the button on the given framebuffer at the given coordinates.
     * Precondition: set_button_type() has been called, otherwise result is no-op
     *
     * @param buffer The target framebuffer
     * @param geometry The geometry of the button, in logical coordinates
     * @param scissor The scissor rectangle to render.
     */
    void render(const wf::framebuffer_t& buffer, wf::geometry_t geometry,
        wf::geometry_t scissor);

  private:
    const decoration_theme_t& theme;

    /* Whether the button needs repaint */
    bool damaged = false;
    button_type_t type;
    uint32_t button_texture = -1;

    /**
     * Redraw the button surface and store it as a texture
     */
    void update_texture();
};
}
}
