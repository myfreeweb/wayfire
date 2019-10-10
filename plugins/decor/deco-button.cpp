#include "deco-button.hpp"
#include "deco-theme.hpp"
#include <wayfire/opengl.hpp>
#include <cairo-util.hpp>

#define HOVERED  1.0
#define NORMAL   0.0
#define PRESSED -0.7

namespace wf
{
namespace decor
{

button_t::button_t(const decoration_theme_t& t, std::function<void()> damage)
    : theme(t), damage_callback(damage)
{ }

void button_t::set_button_type(button_type_t type)
{
    this->type = type;
    this->hover.animate(0, 0);
    update_texture();
    add_idle_damage();
}

button_type_t button_t::get_button_type() const
{
    return this->type;
}

void button_t::set_hover(bool is_hovered)
{
    this->is_hovered = is_hovered;
    if (!this->is_pressed)
    {
        if (is_hovered) {
            this->hover.animate(HOVERED);
        } else {
            this->hover.animate(NORMAL);
        }
    }

    add_idle_damage();
}
/**
 * Set whether the button is pressed or not.
 * Affects appearance.
 */
void button_t::set_pressed(bool is_pressed)
{
    this->is_pressed = is_pressed;
    if (is_pressed) {
        this->hover.animate(PRESSED);
    } else {
        this->hover.animate(is_hovered ? HOVERED : NORMAL);
    }

    add_idle_damage();
}

void button_t::render(const wf::framebuffer_t& fb, wf::geometry_t geometry,
    wf::geometry_t scissor)
{
    assert(this->button_texture != uint32_t(-1));

    OpenGL::render_begin(fb);
    fb.scissor(scissor);

    gl_geometry gg;
    gg.x1 = geometry.x + fb.geometry.x;
    gg.y1 = geometry.y + fb.geometry.y;
    gg.x2 = gg.x1 + geometry.width;
    gg.y2 = gg.y1 + geometry.height;

    OpenGL::render_transformed_texture(button_texture, gg, {},
        fb.get_orthographic_projection(), {1, 1, 1, 1},
        TEXTURE_TRANSFORM_INVERT_Y);

    OpenGL::render_end();

    if (this->hover.running())
        add_idle_damage();
}

void button_t::update_texture()
{
    /* We render a big predefined resolution here */
    const int WIDTH = 25;
    const int HEIGHT = 16;
    const int BORDER = 1;
    const int SCALE = 4;

    decoration_theme_t::button_state_t state = {
        .width = WIDTH * SCALE,
        .height = HEIGHT * SCALE,
        .border = BORDER * SCALE,
        .hover_progress = hover,
    };

    auto surface = theme.get_button_surface(type, state);
    OpenGL::render_begin();
    cairo_surface_upload_to_texture(surface, this->button_texture);
    OpenGL::render_end();
}

void button_t::add_idle_damage()
{
    this->idle_damage.run_once([=] () {
        this->damage_callback();
        update_texture();
    });
}

}
}
