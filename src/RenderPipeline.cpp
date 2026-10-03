#include "RenderPipeline.h"

#include <algorithm>
#include <array>
#include <glm/gtc/matrix_transform.hpp>

RenderPipeline::~RenderPipeline() {
  destroy_targets();
  if (shadow_fbo_ != 0) {
    glDeleteFramebuffers(1, &shadow_fbo_);
    glDeleteTextures(1, &shadow_tex_);
  }
  if (empty_vao_ != 0) {
    glDeleteVertexArrays(1, &empty_vao_);
  }
}

bool RenderPipeline::init(Renderer& renderer,
                          const std::filesystem::path& shader_dir) {
  const auto pbr{shader_dir / "pbr_shaders.glsl"};
  const auto post{shader_dir / "post_fx.glsl"};
  bool ok{true};
  ok &= renderer.load_shader_sections("pbr", pbr, "scene.vert", "pbr.frag");
  ok &= renderer.load_shader_sections("shadow", pbr, "depth.vert", "depth.frag");
  ok &= renderer.load_shader_sections("highlight", pbr, "scene.vert", "highlight.frag");
  ok &= renderer.load_shader_sections("ssao", post, "fullscreen.vert", "ssao.frag");
  ok &= renderer.load_shader_sections("ao_blur", post, "fullscreen.vert", "ao_blur.frag");
  ok &= renderer.load_shader_sections("composite", post, "fullscreen.vert", "composite.frag");
  if (!ok) {
    return false;
  }

  GLint max_samples{};
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  samples_ = std::clamp(max_samples, 1, 4);

  glGenVertexArrays(1, &empty_vao_);

  // Shadow map with hardware depth comparison (sampler2DShadow): every tap
  // returns a bilinearly filtered 2x2 PCF result, the shader adds 16 rotated
  // Poisson taps on top for wide soft penumbras.
  glGenFramebuffers(1, &shadow_fbo_);
  glGenTextures(1, &shadow_tex_);
  glBindTexture(GL_TEXTURE_2D, shadow_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, k_shadow_size,
               k_shadow_size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  constexpr std::array<float, 4> border{1.0F, 1.0F, 1.0F, 1.0F};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         shadow_tex_, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  const bool shadow_ok{glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                       GL_FRAMEBUFFER_COMPLETE};
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!shadow_ok) {
    LOG("GL", "Shadow framebuffer incomplete");
    return false;
  }
  set_light_direction({-0.42F, 1.0F, -0.55F});
  return true;
}

void RenderPipeline::set_light_direction(const glm::vec3& towards_light) {
  const glm::vec3 dir{normalize(towards_light)};
  light_view_ = lookAt(dir * 90.0F, glm::vec3{0.0F}, {0.0F, 1.0F, 0.0F});
  // Tight box around the board and the near part of the table so the 4096²
  // map gives ~0.02 world units per texel.
  light_proj_ = glm::ortho(-42.0F, 42.0F, -42.0F, 42.0F, 20.0F, 170.0F);
}

namespace {
GLuint make_texture(GLint internal, int w, int h, GLenum format, GLenum type,
                    GLint filter) {
  GLuint tex{};
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return tex;
}

GLuint make_renderbuffer(GLenum internal, int w, int h, int samples) {
  GLuint rb{};
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  if (samples > 1) {
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, w, h);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, internal, w, h);
  }
  return rb;
}

void check_fbo(const char* name) {
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    LOGF("GL", "Framebuffer '{}' incomplete", name);
  }
}
}  // namespace

void RenderPipeline::resize(int width, int height) {
  width = std::max(width, 1);
  height = std::max(height, 1);
  if (width == width_ && height == height_) {
    return;
  }
  width_ = width;
  height_ = height;
  destroy_targets();
  create_targets();
}

void RenderPipeline::create_targets() {
  const int w{width_};
  const int h{height_};
  const int hw{std::max(w / 2, 1)};
  const int hh{std::max(h / 2, 1)};

  // MSAA HDR scene target (stencil is used for piece outlines).
  glGenFramebuffers(1, &msaa_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, msaa_fbo_);
  msaa_color_rb_ = make_renderbuffer(GL_RGBA16F, w, h, samples_);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, msaa_color_rb_);
  msaa_depth_rb_ = make_renderbuffer(GL_DEPTH24_STENCIL8, w, h, samples_);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, msaa_depth_rb_);
  check_fbo("msaa");

  // Resolved colour + depth textures read by the post passes.
  glGenFramebuffers(1, &resolve_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo_);
  scene_color_tex_ = make_texture(GL_RGBA16F, w, h, GL_RGBA, GL_FLOAT, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         scene_color_tex_, 0);
  scene_depth_tex_ = make_texture(GL_DEPTH24_STENCIL8, w, h, GL_DEPTH_STENCIL,
                                  GL_UNSIGNED_INT_24_8, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, scene_depth_tex_, 0);
  check_fbo("resolve");

  // Half-res planar reflection (alpha marks where a piece was drawn).
  glGenFramebuffers(1, &reflect_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, reflect_fbo_);
  reflect_tex_ = make_texture(GL_RGBA16F, hw, hh, GL_RGBA, GL_FLOAT, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         reflect_tex_, 0);
  reflect_depth_rb_ = make_renderbuffer(GL_DEPTH_COMPONENT24, hw, hh, 1);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, reflect_depth_rb_);
  check_fbo("reflection");

  // Half-res SSAO ping-pong.
  for (int i = 0; i < 2; ++i) {
    glGenFramebuffers(1, &ao_fbo_[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, ao_fbo_[i]);
    ao_tex_[i] = make_texture(GL_R8, hw, hh, GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           ao_tex_[i], 0);
    check_fbo("ao");
  }

  // Integer id buffer for mouse picking.
  glGenFramebuffers(1, &pick_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, pick_fbo_);
  pick_tex_ = make_texture(GL_R32I, w, h, GL_RED_INTEGER, GL_INT, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         pick_tex_, 0);
  pick_depth_rb_ = make_renderbuffer(GL_DEPTH_COMPONENT24, w, h, 1);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, pick_depth_rb_);
  check_fbo("picking");

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void RenderPipeline::destroy_targets() {
  const auto del_fbo = [](GLuint& id) {
    if (id != 0) {
      glDeleteFramebuffers(1, &id);
      id = 0;
    }
  };
  const auto del_tex = [](GLuint& id) {
    if (id != 0) {
      glDeleteTextures(1, &id);
      id = 0;
    }
  };
  const auto del_rb = [](GLuint& id) {
    if (id != 0) {
      glDeleteRenderbuffers(1, &id);
      id = 0;
    }
  };
  del_fbo(msaa_fbo_);
  del_rb(msaa_color_rb_);
  del_rb(msaa_depth_rb_);
  del_fbo(resolve_fbo_);
  del_tex(scene_color_tex_);
  del_tex(scene_depth_tex_);
  del_fbo(reflect_fbo_);
  del_tex(reflect_tex_);
  del_rb(reflect_depth_rb_);
  for (int i = 0; i < 2; ++i) {
    del_fbo(ao_fbo_[i]);
    del_tex(ao_tex_[i]);
  }
  del_fbo(pick_fbo_);
  del_tex(pick_tex_);
  del_rb(pick_depth_rb_);
}

void RenderPipeline::begin_shadow_pass() {
  glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
  glViewport(0, 0, k_shadow_size, k_shadow_size);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  // Front-face culling pushes the stored depth to back faces -> less acne.
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5F, 2.0F);
}

void RenderPipeline::end_shadow_pass() {
  glDisable(GL_POLYGON_OFFSET_FILL);
  glCullFace(GL_BACK);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
}

void RenderPipeline::begin_reflection_pass() {
  glBindFramebuffer(GL_FRAMEBUFFER, reflect_fbo_);
  glViewport(0, 0, std::max(width_ / 2, 1), std::max(height_ / 2, 1));
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_CLIP_DISTANCE0);
  glFrontFace(GL_CW);  // mirroring flips the winding order
}

void RenderPipeline::end_reflection_pass() {
  glFrontFace(GL_CCW);
  glDisable(GL_CLIP_DISTANCE0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
}

void RenderPipeline::begin_scene_pass() {
  glBindFramebuffer(GL_FRAMEBUFFER, msaa_fbo_);
  glViewport(0, 0, width_, height_);
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);  // alpha 0 = backdrop shows through
  glClearStencil(0);
  glStencilMask(0xFF);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 0, 0xFF);
  glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glEnable(GL_MULTISAMPLE);
}

void RenderPipeline::end_scene_pass() {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_);
  glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_,
                    GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_STENCIL_TEST);
}

void RenderPipeline::begin_picking_pass() {
  glBindFramebuffer(GL_FRAMEBUFFER, pick_fbo_);
  glViewport(0, 0, width_, height_);
  constexpr std::array<GLint, 4> clear{-1, -1, -1, -1};
  glClearBufferiv(GL_COLOR, 0, clear.data());
  constexpr GLfloat depth{1.0F};
  glClearBufferfv(GL_DEPTH, 0, &depth);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
}

int RenderPipeline::end_picking_pass(const glm::ivec2& pixel) {
  int id{-1};
  if (pixel.x >= 0 && pixel.y >= 0 && pixel.x < width_ && pixel.y < height_) {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(pixel.x, pixel.y, 1, 1, GL_RED_INTEGER, GL_INT, &id);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return id;
}

void RenderPipeline::bind_scene_inputs(Renderer& renderer,
                                       bool with_reflection) const {
  glActiveTexture(GL_TEXTURE0 + k_shadow_unit);
  glBindTexture(GL_TEXTURE_2D, shadow_tex_);
  renderer.set_shader_uniform("shadow_map", k_shadow_unit);
  glActiveTexture(GL_TEXTURE0 + k_reflection_unit);
  glBindTexture(GL_TEXTURE_2D, with_reflection ? reflect_tex_ : 0);
  renderer.set_shader_uniform("reflection_tex", k_reflection_unit);
  renderer.set_shader_uniform("light_space", light_space());
  renderer.set_shader_uniform(
      "viewport_size", glm::vec2{static_cast<float>(width_), static_cast<float>(height_)});
  glActiveTexture(GL_TEXTURE0);
}

void RenderPipeline::run_post(Renderer& renderer, const glm::mat4& projection,
                              float time, const PostSettings& s) {
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glBindVertexArray(empty_vao_);
  const int hw{std::max(width_ / 2, 1)};
  const int hh{std::max(height_ / 2, 1)};
  const glm::vec2 texel{1.0F / static_cast<float>(width_),
                        1.0F / static_cast<float>(height_)};

  // 1) SSAO at half resolution.
  glBindFramebuffer(GL_FRAMEBUFFER, ao_fbo_[0]);
  glViewport(0, 0, hw, hh);
  renderer.install_shader("ssao");
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_depth_tex_);
  renderer.set_shader_uniform("depth_tex", 0);
  renderer.set_shader_uniform("inv_projection", inverse(projection));
  renderer.set_shader_uniform("projection", projection);
  renderer.set_shader_uniform("depth_texel", texel);
  renderer.set_shader_uniform("ao_radius", s.ao_radius);
  renderer.set_shader_uniform("ao_intensity", s.ssao ? s.ao_intensity : 0.0F);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // 2) Depth-aware blur.
  glBindFramebuffer(GL_FRAMEBUFFER, ao_fbo_[1]);
  renderer.install_shader("ao_blur");
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, ao_tex_[0]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, scene_depth_tex_);
  renderer.set_shader_uniform("ao_tex", 0);
  renderer.set_shader_uniform("depth_tex", 1);
  renderer.set_shader_uniform(
      "ao_texel", glm::vec2{1.0F / static_cast<float>(hw), 1.0F / static_cast<float>(hh)});
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // 3) Composite to the default framebuffer.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
  renderer.install_shader("composite");
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_color_tex_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, scene_depth_tex_);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, ao_tex_[1]);
  renderer.set_shader_uniform("scene_tex", 0);
  renderer.set_shader_uniform("depth_tex", 1);
  renderer.set_shader_uniform("ao_tex", 2);
  renderer.set_shader_uniform("texel", texel);
  renderer.set_shader_uniform("near_plane", Camera::k_near);
  renderer.set_shader_uniform("far_plane", Camera::k_far);
  renderer.set_shader_uniform("focus_distance", s.focus_distance);
  renderer.set_shader_uniform("focus_range", s.focus_range);
  renderer.set_shader_uniform("dof_scale", s.dof_scale);
  renderer.set_shader_uniform("max_coc", s.dof ? s.max_coc : 0.0F);
  renderer.set_shader_uniform("ao_strength", s.ssao ? s.ao_strength : 0.0F);
  renderer.set_shader_uniform("exposure", s.exposure);
  renderer.set_shader_uniform("time", time);
  renderer.set_shader_uniform("backdrop_top", s.backdrop_top);
  renderer.set_shader_uniform("backdrop_bottom", s.backdrop_bottom);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, backdrop_tex_);
  renderer.set_shader_uniform("backdrop_tex", 3);
  renderer.set_shader_uniform("backdrop_mode", backdrop_tex_ != 0 ? s.backdrop_mode : 0);
  renderer.set_shader_uniform("backdrop_aspect", backdrop_aspect_);
  renderer.set_shader_uniform("screen_aspect",
                              static_cast<float>(width_) / static_cast<float>(height_));
  renderer.set_shader_uniform("backdrop_vp", backdrop_vp_);
  renderer.set_shader_uniform("horizon_y", s.horizon_y);
  renderer.set_shader_uniform("backdrop_shift_u", s.backdrop_shift_u);
  renderer.set_shader_uniform("backdrop_gain", s.backdrop_gain);
  renderer.set_shader_uniform("vignette", s.vignette);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
}

std::vector<unsigned char> RenderPipeline::read_back_buffer() const {
  std::vector<unsigned char> pixels(static_cast<size_t>(width_) * height_ * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(GL_BACK);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  // Flip vertically: GL's origin is bottom-left, PNG's is top-left.
  const size_t row{static_cast<size_t>(width_) * 4};
  std::vector<unsigned char> tmp(row);
  for (int y = 0; y < height_ / 2; ++y) {
    unsigned char* a{pixels.data() + static_cast<size_t>(y) * row};
    unsigned char* b{pixels.data() + static_cast<size_t>(height_ - 1 - y) * row};
    std::copy(a, a + row, tmp.data());
    std::copy(b, b + row, a);
    std::copy(tmp.data(), tmp.data() + row, b);
  }
  return pixels;
}
