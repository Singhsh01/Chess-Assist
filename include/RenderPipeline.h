#pragma once
// RenderPipeline.h — offscreen targets and passes for the PBR renderer.
//
// Frame structure:
//   1. shadow pass      depth-only, 4096² directional light map (PCF in shader)
//   2. reflection pass  pieces mirrored about the board plane, half-res HDR
//   3. scene pass       4x MSAA RGBA16F + depth/stencil, resolved to textures
//   4. post             SSAO (half res) -> depth-aware blur -> composite
//                       (DOF gather, AO, backdrop, ACES, vignette, grain)
//   5. picking          R32I id buffer for mouse selection (on demand)

#include <glad/gl.h>

#include <filesystem>
#include <vector>

#include "renderer.hpp"

struct PostSettings {
  float focus_distance{60.0F};
  float focus_range{24.0F};
  float dof_scale{55.0F};
  float max_coc{14.0F};
  float ao_strength{0.85F};
  float ao_radius{2.2F};
  float ao_intensity{1.6F};
  float exposure{1.05F};
  glm::vec3 backdrop_top{0.060F, 0.062F, 0.070F};
  glm::vec3 backdrop_bottom{0.006F, 0.006F, 0.008F};
  bool dof{true};
  bool ssao{true};
  // Backdrop behind the scene: 0 = studio gradient, 1 = gallery photo.
  int backdrop_mode{0};
  float horizon_y{1.2F};       // screen-space horizon height (0..1)
  float backdrop_shift_u{0.0F};
  float backdrop_gain{1.25F};
  float vignette{0.72F};
};

class RenderPipeline {
 public:
  static constexpr int k_shadow_size{4096};
  static constexpr GLint k_shadow_unit{3};
  static constexpr GLint k_reflection_unit{4};

  RenderPipeline() = default;
  ~RenderPipeline();
  RenderPipeline(const RenderPipeline&) = delete;
  RenderPipeline& operator=(const RenderPipeline&) = delete;
  RenderPipeline(RenderPipeline&&) = delete;
  RenderPipeline& operator=(RenderPipeline&&) = delete;

  bool init(Renderer& renderer, const std::filesystem::path& shader_dir);
  void resize(int width, int height);  // framebuffer pixels
  [[nodiscard]] glm::ivec2 size() const { return {width_, height_}; }

  // Light ------------------------------------------------------------------
  void set_light_direction(const glm::vec3& towards_light);
  [[nodiscard]] const glm::mat4& light_projection() const { return light_proj_; }
  [[nodiscard]] const glm::mat4& light_view() const { return light_view_; }
  [[nodiscard]] glm::mat4 light_space() const { return light_proj_ * light_view_; }

  // Passes -------------------------------------------------------------------
  void begin_shadow_pass();
  void end_shadow_pass();
  void begin_reflection_pass();
  void end_reflection_pass();
  void begin_scene_pass();
  void end_scene_pass();
  void begin_picking_pass();
  int end_picking_pass(const glm::ivec2& pixel);  // returns id or -1
  void run_post(Renderer& renderer, const glm::mat4& projection, float time,
                const PostSettings& settings);

  // Gallery photo used as the backdrop (texture owned by the Renderer).
  void set_backdrop(GLuint texture, float aspect, const glm::vec2& vanishing_point) {
    backdrop_tex_ = texture;
    backdrop_aspect_ = aspect;
    backdrop_vp_ = vanishing_point;
  }
  [[nodiscard]] bool has_backdrop() const { return backdrop_tex_ != 0; }

  // Binds the shadow map / reflection texture for the currently installed
  // PBR program and sets their sampler uniforms.
  void bind_scene_inputs(Renderer& renderer, bool with_reflection) const;

  // Reads back the default framebuffer (after post + HUD) as RGBA, top row
  // first, for PNG screenshots.
  [[nodiscard]] std::vector<unsigned char> read_back_buffer() const;

 private:
  void destroy_targets();
  void create_targets();

  int width_{};
  int height_{};
  int samples_{4};

  GLuint shadow_fbo_{};
  GLuint shadow_tex_{};

  GLuint msaa_fbo_{};
  GLuint msaa_color_rb_{};
  GLuint msaa_depth_rb_{};

  GLuint resolve_fbo_{};
  GLuint scene_color_tex_{};
  GLuint scene_depth_tex_{};

  GLuint reflect_fbo_{};
  GLuint reflect_tex_{};
  GLuint reflect_depth_rb_{};

  GLuint ao_fbo_[2]{};
  GLuint ao_tex_[2]{};

  GLuint pick_fbo_{};
  GLuint pick_tex_{};
  GLuint pick_depth_rb_{};

  GLuint empty_vao_{};
  GLuint backdrop_tex_{};
  float backdrop_aspect_{1.0F};
  glm::vec2 backdrop_vp_{0.5F, 0.5F};

  glm::mat4 light_proj_{1.0F};
  glm::mat4 light_view_{1.0F};
};
