#include "deco-button.hpp"
#include "deco-theme.hpp"
#include <wayfire/opengl.hpp>
#include <cairo-util.hpp>

namespace wf
{
namespace decor
{

button_t::button_t(const decoration_theme_t& t)
    : theme(t) {}

void button_t::set_button_type(button_type_t type)
{
    this->type = type;
    update_texture();
}

button_type_t button_t::get_button_type() const
{
    return this->type;
}

void button_t::set_hover(bool is_hovered)
{
    // TODO
}

bool button_t::needs_repaint()
{
    return true;
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
}

void button_t::update_texture()
{
    // XXX: we render at a predefined resolution here ...
    const int WIDTH = 60;
    const int HEIGHT = 30;

    auto surface = theme.get_button_surface(type, WIDTH, HEIGHT);
    OpenGL::render_begin();
    cairo_surface_upload_to_texture(surface, this->button_texture);
    OpenGL::render_end();
}

}
}
