#pragma once

#include <cairo.h>
#include <wayfire/opengl.hpp>

/**
 * Upload the data from the surface to the OpenGL texture.
 * If @tex is -1, a new texture will be generated.
 */
static void cairo_surface_upload_to_texture(
    cairo_surface_t *surface, GLuint& tex)
{
    if (tex == (uint32_t)-1) {
        GL_CALL(glGenTextures(1, &tex));
    }

    auto src = cairo_image_surface_get_data(surface);
    GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));

    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            cairo_image_surface_get_width(surface),
            cairo_image_surface_get_height(surface),
            0, GL_RGBA, GL_UNSIGNED_BYTE, src));
}

/**
 * Render the given texture at the given geometry.
 */
static void render_gl_texture(const wf::framebuffer_t& fb,
    wf::geometry_t geometry, GLuint texture)
{
    gl_geometry gg;
    gg.x1 = geometry.x + fb.geometry.x;
    gg.y1 = geometry.y + fb.geometry.y;
    gg.x2 = gg.x1 + geometry.width;
    gg.y2 = gg.y1 + geometry.height;

    OpenGL::render_transformed_texture(texture, gg, {},
        fb.get_orthographic_projection(), {1, 1, 1, 1},
        TEXTURE_TRANSFORM_INVERT_Y);
}
