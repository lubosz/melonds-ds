/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include "opengl.hpp"

#include <gfx/gl_capabilities.h>
#include <libretro.h>
#include <glsm/glsm.h>
#include <retro_assert.h>

#include <GPU.h>
#include <OpenGLSupport.h>

#include "embedded/melondsds_fragment_shader.h"
#include "embedded/melondsds_vertex_shader.h"
#include "PlatformOGLPrivate.h"
#include "screenlayout.hpp"
#include "input.hpp"
#include "environment.hpp"
#include "config.hpp"
#include "render.hpp"

// HACK: Defined in glsm.c, but we need to peek into it occasionally
extern struct retro_hw_render_callback hw_render;

static const char* const SHADER_PROGRAM_NAME = "melonDS DS Shader Program";

namespace melonds::opengl {
    // TODO: Introduce a OpenGlState struct to hold all of these variables
    static bool openGlDebugAvailable = false;
    bool refresh_opengl = true;
    static bool context_initialized = false;
    static GLuint shader[3];
    static GLuint screen_framebuffer_texture;
    static float screen_vertices[72];
    static GLuint vao, vbo;

    static struct {
        GLfloat uScreenSize[2];
        u32 u3DScale;
        u32 uFilterMode;
        GLfloat cursorPos[4];

    } GL_ShaderConfig;
    static GLuint ubo;

    static void ContextReset() noexcept;

    static void context_destroy();

    static bool SetupOpenGl() noexcept;

    static void InitializeFrameState(const ScreenLayoutData& screenLayout) noexcept;
}

bool melonds::opengl::ContextInitialized() {
    return context_initialized;
}

bool melonds::opengl::UsingOpenGl() {
    if (melonds::render::CurrentRenderer() == melonds::Renderer::OpenGl) {
        return true;
    }

    return false;
}

void melonds::opengl::RequestOpenGlRefresh() {
    refresh_opengl = true;
}

bool melonds::opengl::Initialize() noexcept {
    retro::log(RETRO_LOG_DEBUG, "melonds::opengl::Initialize()");
    glsm_ctx_params_t params = {};

    // melonds wants an opengl 3.1 context, so glcore is required for mesa compatibility
    params.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    params.major = 3;
    params.minor = 1;
    params.context_reset = ContextReset;
    params.context_destroy = context_destroy;
    params.environ_cb = retro::environment;
    params.stencil = false;
    params.framebuffer_lock = nullptr;

#ifndef NDEBUG
    hw_render.debug_context = true;
#endif

    bool ok = glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params);

#ifndef NDEBUG
    retro_assert(hw_render.debug_context);
#endif

    gl_query_core_context_set(hw_render.context_type == RETRO_HW_CONTEXT_OPENGL_CORE || hw_render.context_type == RETRO_HW_CONTEXT_OPENGL);

    return ok;
}

void melonds::opengl::Render(const InputState& state, const ScreenLayoutData& screenLayout) noexcept {
    retro_assert(melonds::render::CurrentRenderer() == melonds::Renderer::OpenGl);
    glsm_ctl(GLSM_CTL_STATE_BIND, nullptr);

    int frontbuf = GPU::FrontBuffer;
    bool virtual_cursor = state.CursorEnabled();

    // Tell OpenGL that we want to draw to (and read from) the screen framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, glsm_get_current_framebuffer());

    if (refresh_opengl) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        InitializeFrameState(screenLayout);
    }

    if (virtual_cursor) {
        float cursorSize = melonds::config::video::CursorSize();
        GL_ShaderConfig.cursorPos[0] = ((float) (state.TouchX()) - cursorSize) / (NDS_SCREEN_HEIGHT * 1.35f);
        GL_ShaderConfig.cursorPos[1] = (((float) (state.TouchY()) - cursorSize) / (NDS_SCREEN_WIDTH * 1.5f)) + 0.5f;
        GL_ShaderConfig.cursorPos[2] = ((float) (state.TouchX()) + cursorSize) / (NDS_SCREEN_HEIGHT * 1.35f);
        GL_ShaderConfig.cursorPos[3] = (((float) (state.TouchY()) + cursorSize) / ((float) NDS_SCREEN_WIDTH * 1.5f)) + 0.5f;

        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        void *unibuf = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
        if (unibuf) memcpy(unibuf, &GL_ShaderConfig, sizeof(GL_ShaderConfig));
        glUnmapBuffer(GL_UNIFORM_BUFFER);
    }

    OpenGL::UseShaderProgram(shader);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    glViewport(0, 0, screenLayout.BufferWidth(), screenLayout.BufferHeight());

    glActiveTexture(GL_TEXTURE0);

    GPU::CurGLCompositor->BindOutputTexture(frontbuf);

    // Set the filtering mode for the active texture
    // For simplicity, we'll just use the same filter for both minification and magnification
    GLint filter = config::video::ScreenFilter() == ScreenFilter::Linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0,
                 screenLayout.HybridSmallScreenLayout() == SmallScreenLayout::SmallScreenDuplicate ? 18 : 12);

    glFlush();

    glsm_ctl(GLSM_CTL_STATE_UNBIND, nullptr);

    retro::video_refresh(
        RETRO_HW_FRAME_BUFFER_VALID,
        screenLayout.BufferWidth(),
        screenLayout.BufferHeight(),
        0
    );

}


void melonds::opengl::deinitialize() {
    retro::log(RETRO_LOG_DEBUG, "melonds::opengl::deinitialize()");
    GPU::DeInitRenderer();
    GPU::InitRenderer(false);
}

static void melonds::opengl::ContextReset() noexcept {
    retro::debug("melonds::opengl::ContextReset()");
    if (UsingOpenGl() && GPU3D::CurrentRenderer) { // If we're using OpenGL, but there's already a renderer in place...
        retro::debug("GPU3D renderer is assigned; deinitializing it before resetting the context.");
        GPU::DeInitRenderer();
    }

    // Initialize all OpenGL function pointers
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, nullptr);

    // Initialize global OpenGL resources (e.g. VAOs) and get config info (e.g. limits)
    glsm_ctl(GLSM_CTL_STATE_SETUP, nullptr);

    // Start using global OpenGL structures
    glsm_ctl(GLSM_CTL_STATE_BIND, nullptr);

    GPU::InitRenderer(static_cast<int>(melonds::render::CurrentRenderer()));

    context_initialized = SetupOpenGl();

    // Stop using OpenGL structures
    glsm_ctl(GLSM_CTL_STATE_UNBIND, nullptr); // Always succeeds

    if (context_initialized) {
        retro::debug("OpenGL context reset successfully.");
    } else {
        retro::error("OpenGL context reset failed.");
    }
}

static void melonds::opengl::context_destroy() {
    retro::log(RETRO_LOG_DEBUG, "melonds::opengl::context_destroy()");
    glsm_ctl(GLSM_CTL_STATE_BIND, nullptr);
    glDeleteTextures(1, &screen_framebuffer_texture);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    OpenGL::DeleteShaderProgram(shader);
    glsm_ctl(GLSM_CTL_STATE_UNBIND, nullptr);
}

// Sets up OpenGL resources specific to melonDS
static bool melonds::opengl::SetupOpenGl() noexcept {
    retro::debug("melonds::opengl::SetupOpenGl()");

    openGlDebugAvailable = gl_check_capability(GL_CAPS_DEBUG);
    if (openGlDebugAvailable) {
        retro::debug("OpenGL debugging extensions are available");
    }

    if (!OpenGL::BuildShaderProgram(embedded_melondsds_vertex_shader, embedded_melondsds_fragment_shader, shader, SHADER_PROGRAM_NAME))
        return false;

    if (openGlDebugAvailable) {
        glObjectLabel(GL_SHADER, shader[0], -1, "melonDS DS Vertex Shader");
        glObjectLabel(GL_SHADER, shader[1], -1, "melonDS DS Fragment Shader");
        glObjectLabel(GL_PROGRAM, shader[2], -1, SHADER_PROGRAM_NAME);
    }

    glBindAttribLocation(shader[2], 0, "vPosition");
    glBindAttribLocation(shader[2], 1, "vTexcoord");
    glBindFragDataLocation(shader[2], 0, "oColor");

    if (!OpenGL::LinkShaderProgram(shader))
        return false;

    GLuint uConfigBlockIndex = glGetUniformBlockIndex(shader[2], "uConfig");
    glUniformBlockBinding(shader[2], uConfigBlockIndex, 16); // TODO: Where does 16 come from? It's not a size.

    glUseProgram(shader[2]);
    GLuint uni_id = glGetUniformLocation(shader[2], "ScreenTex");
    glUniform1i(uni_id, 0);

    memset(&GL_ShaderConfig, 0, sizeof(GL_ShaderConfig));

    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(GL_ShaderConfig), &GL_ShaderConfig, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 16, ubo);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(screen_vertices), nullptr, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * 4, (void *) nullptr);
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * 4, (void *) (2 * 4));

    glGenTextures(1, &screen_framebuffer_texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screen_framebuffer_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 256 * 3 + 1, 192 * 2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);

    refresh_opengl = true;

    return true;
}

void melonds::opengl::InitializeFrameState(const ScreenLayoutData& screenLayout) noexcept {

    refresh_opengl = false;
    GPU::RenderSettings render_settings = melonds::config::video::RenderSettings();
    GPU::SetRenderSettings(static_cast<int>(Renderer::OpenGl), render_settings);

    GL_ShaderConfig.uScreenSize[0] = (float) screenLayout.BufferWidth();
    GL_ShaderConfig.uScreenSize[1] = (float) screenLayout.BufferHeight();
    GL_ShaderConfig.u3DScale = config::video::ScaleFactor();
    GL_ShaderConfig.cursorPos[0] = -1.0f;
    GL_ShaderConfig.cursorPos[1] = -1.0f;
    GL_ShaderConfig.cursorPos[2] = -1.0f;
    GL_ShaderConfig.cursorPos[3] = -1.0f;

    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    void *unibuf = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
    if (unibuf) memcpy(unibuf, &GL_ShaderConfig, sizeof(GL_ShaderConfig));
    glUnmapBuffer(GL_UNIFORM_BUFFER);

    float screen_width = (float) screenLayout.ScreenWidth();
    float screen_height = (float) screenLayout.ScreenHeight();
    float screen_gap = (float) screenLayout.ScaledScreenGap();

    float top_screen_x = 0.0f;
    float top_screen_y = 0.0f;
    float top_screen_scale = 1.0f;

    float bottom_screen_x = 0.0f;
    float bottom_screen_y = 0.0f;
    float bottom_screen_scale = 1.0f;

    float primary_x = 0.0f;
    float primary_y = 0.0f;
    float primary_tex_v0_x = 0.0f;
    float primary_tex_v0_y = 0.0f;
    float primary_tex_v1_x = 0.0f;
    float primary_tex_v1_y = 0.0f;
    float primary_tex_v2_x = 0.0f;
    float primary_tex_v2_y = 0.0f;
    float primary_tex_v3_x = 0.0f;
    float primary_tex_v3_y = 0.0f;
    float primary_tex_v4_x = 0.0f;
    float primary_tex_v4_y = 0.0f;
    float primary_tex_v5_x = 0.0f;
    float primary_tex_v5_y = 0.0f;

    const float pixel_pad = 1.0f / (192 * 2 + 2);

    // TODO: Implement rotated and upside-down layouts
    switch (screenLayout.Layout()) {
        case ScreenLayout::TopBottom:
            bottom_screen_y = screen_height + screen_gap;
            break;
        case ScreenLayout::BottomTop:
            top_screen_y = screen_height + screen_gap;
            break;
        case ScreenLayout::LeftRight:
            bottom_screen_x = screen_width;
            break;
        case ScreenLayout::RightLeft:
            top_screen_x = screen_width;
            break;
        case ScreenLayout::TopOnly:
            bottom_screen_y = screen_height; // Meh, let's just hide it
            break;
        case ScreenLayout::BottomOnly:
            top_screen_y = screen_height; // ditto
            break;
        case ScreenLayout::HybridTop:
            primary_x = screen_width * screenLayout.HybridRatio();
            primary_y = screen_height * screenLayout.HybridRatio();

            primary_tex_v0_x = 0.0f;
            primary_tex_v0_y = 0.0f;
            primary_tex_v1_x = 0.0f;
            primary_tex_v1_y = 0.5f - pixel_pad;
            primary_tex_v2_x = 1.0f;
            primary_tex_v2_y = 0.5f - pixel_pad;
            primary_tex_v3_x = 0.0f;
            primary_tex_v3_y = 0.0f;
            primary_tex_v4_x = 1.0f;
            primary_tex_v4_y = 0.0f;
            primary_tex_v5_x = 1.0f;
            primary_tex_v5_y = 0.5f - pixel_pad;

            break;
        case ScreenLayout::HybridBottom:
            primary_x = screen_width * screenLayout.HybridRatio();
            primary_y = screen_height * screenLayout.HybridRatio();

            primary_tex_v0_x = 0.0f;
            primary_tex_v0_y = 0.5f + pixel_pad;
            primary_tex_v1_x = 0.0f;
            primary_tex_v1_y = 1.0f;
            primary_tex_v2_x = 1.0f;
            primary_tex_v2_y = 1.0f;
            primary_tex_v3_x = 0.0f;
            primary_tex_v3_y = 0.5f + pixel_pad;
            primary_tex_v4_x = 1.0f;
            primary_tex_v4_y = 0.5f + pixel_pad;
            primary_tex_v5_x = 1.0f;
            primary_tex_v5_y = 01.0;

            break;
    }

#define SETVERTEX(i, x, y, t_x, t_y) \
    do {                               \
        screen_vertices[(4 * i) + 0] = x; \
        screen_vertices[(4 * i) + 1] = y; \
        screen_vertices[(4 * i) + 2] = t_x; \
        screen_vertices[(4 * i) + 3] = t_y; \
    } while (false)

    ScreenLayout layout = screenLayout.Layout();
    SmallScreenLayout smallScreenLayout = screenLayout.HybridSmallScreenLayout();
    if (screenLayout.IsHybridLayout()) {
        //Primary Screen
        SETVERTEX(0, 0.0f, 0.0f, primary_tex_v0_x, primary_tex_v0_y); // top left
        SETVERTEX(1, 0.0f, primary_y, primary_tex_v1_x, primary_tex_v1_y); // bottom left
        SETVERTEX(2, primary_x, primary_y, primary_tex_v2_x, primary_tex_v2_y); // bottom right
        SETVERTEX(3, 0.0f, 0.0f, primary_tex_v3_x, primary_tex_v3_y); // top left
        SETVERTEX(4, primary_x, 0.0f, primary_tex_v4_x, primary_tex_v4_y); // top right
        SETVERTEX(5, primary_x, primary_y, primary_tex_v5_x, primary_tex_v5_y); // bottom right

        //Top screen
        if (smallScreenLayout == SmallScreenLayout::SmallScreenTop && layout == ScreenLayout::HybridTop) {
            SETVERTEX(6, primary_x, 0.0f, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(7, primary_x, 0.0f + screen_height, 0.0f, 1.0f); // bottom left
            SETVERTEX(8, primary_x + screen_width, 0.0f + screen_height, 1.0f, 1.0f); // bottom right
            SETVERTEX(9, primary_x, 0.0f, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(10, primary_x + screen_width, 0.0f, 1.0f, 0.5f + pixel_pad); // top right
            SETVERTEX(11, primary_x + screen_width, 0.0f + screen_height, 1.0f, 1.0f); // bottom right
        } else if (smallScreenLayout == SmallScreenLayout::SmallScreenDuplicate
                   || (layout == ScreenLayout::HybridBottom && smallScreenLayout == SmallScreenLayout::SmallScreenTop)) {
            SETVERTEX(6, primary_x, 0.0f, 0.0f, 0.0f); // top left
            SETVERTEX(7, primary_x, 0.0f + screen_height, 0.0f, 0.5f - pixel_pad); // bottom left
            SETVERTEX(8, primary_x + screen_width, 0.0f + screen_height, 1.0f, 0.5f - pixel_pad); // bottom right
            SETVERTEX(9, primary_x, 0.0f, 0.0f, 0.0f); // top left
            SETVERTEX(10, primary_x + screen_width, 0.0f, 1.0f, 0.0f); // top right
            SETVERTEX(11, primary_x + screen_width, 0.0f + screen_height, 1.0f, 0.5f - pixel_pad); // bottom right
        }


        //Bottom Screen
        if (smallScreenLayout == SmallScreenLayout::SmallScreenBottom &&
            layout == ScreenLayout::HybridTop) {
            SETVERTEX(6, primary_x, primary_y - screen_height, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(7, primary_x, primary_y, 0.0f, 1.0f); // bottom left
            SETVERTEX(8, primary_x + screen_width, primary_y, 1.0f, 1.0f); // bottom right
            SETVERTEX(9, primary_x, primary_y - screen_height, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(10, primary_x + screen_width, primary_y - screen_height, 1.0f, 0.5f + pixel_pad); // top right
            SETVERTEX(11, primary_x + screen_width, primary_y, 1.0f, 1.0f); // bottom right
        } else if (smallScreenLayout == SmallScreenLayout::SmallScreenBottom && layout == ScreenLayout::HybridBottom) {
            SETVERTEX(6, primary_x, primary_y - screen_height, 0.0f, 0.0f); // top left
            SETVERTEX(7, primary_x, primary_y, 0.0f, 0.5f - pixel_pad); // bottom left
            SETVERTEX(8, primary_x + screen_width, primary_y, 1.0f, 0.5f - pixel_pad); // bottom right
            SETVERTEX(9, primary_x, primary_y - screen_height, 0.0f, 0.0f); // top left
            SETVERTEX(10, primary_x + screen_width, primary_y - screen_height, 1.0f, 0.0f); // top right
            SETVERTEX(11, primary_x + screen_width, primary_y, 1.0f, 0.5f - pixel_pad); // bottom right
        } else if (smallScreenLayout == SmallScreenLayout::SmallScreenDuplicate) {
            SETVERTEX(12, primary_x, primary_y - screen_height, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(13, primary_x, primary_y, 0.0f, 1.0f); // bottom left
            SETVERTEX(14, primary_x + screen_width, primary_y, 1.0f, 1.0f); // bottom right
            SETVERTEX(15, primary_x, primary_y - screen_height, 0.0f, 0.5f + pixel_pad); // top left
            SETVERTEX(16, primary_x + screen_width, primary_y - screen_height, 1.0f, 0.5f + pixel_pad); // top right
            SETVERTEX(17, primary_x + screen_width, primary_y, 1.0f, 1.0f); // bottom right
        }
    } else {
        // top screen
        SETVERTEX(0, top_screen_x, top_screen_y, 0.0f, 0.0f); // top left
        SETVERTEX(1, top_screen_x, top_screen_y + screen_height * top_screen_scale, 0.0f,
                  0.5f - pixel_pad); // bottom left
        SETVERTEX(2, top_screen_x + screen_width * top_screen_scale, top_screen_y + screen_height * top_screen_scale,
                  1.0f, 0.5f - pixel_pad); // bottom right
        SETVERTEX(3, top_screen_x, top_screen_y, 0.0f, 0.0f); // top left
        SETVERTEX(4, top_screen_x + screen_width * top_screen_scale, top_screen_y, 1.0f, 0.0f); // top right
        SETVERTEX(5, top_screen_x + screen_width * top_screen_scale, top_screen_y + screen_height * top_screen_scale,
                  1.0f, 0.5f - pixel_pad); // bottom right

        // bottom screen
        SETVERTEX(6, bottom_screen_x, bottom_screen_y, 0.0f, 0.5f + pixel_pad); // top left
        SETVERTEX(7, bottom_screen_x, bottom_screen_y + screen_height * bottom_screen_scale, 0.0f, 1.0f); // bottom left
        SETVERTEX(8, bottom_screen_x + screen_width * bottom_screen_scale,
                  bottom_screen_y + screen_height * bottom_screen_scale, 1.0f, 1.0f); // bottom right
        SETVERTEX(9, bottom_screen_x, bottom_screen_y, 0.0f, 0.5f + pixel_pad); // top left
        SETVERTEX(10, bottom_screen_x + screen_width * bottom_screen_scale, bottom_screen_y, 1.0f,
                  0.5f + pixel_pad); // top right
        SETVERTEX(11, bottom_screen_x + screen_width * bottom_screen_scale,
                  bottom_screen_y + screen_height * bottom_screen_scale, 1.0f, 1.0f); // bottom right
    }

    // top screen


    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(screen_vertices), screen_vertices);
}
