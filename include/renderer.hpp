#pragma once

#include <glad/gl.h>

#include <deque>
#include <glm/gtc/type_ptr.hpp>
#include <variant>

#include "camera.hpp"

struct Transform {
  glm::vec3 position{};
  float rotation{0.0F};
  float scale{1.0F};
};

struct Vertex {
  glm::vec3 position{};
  glm::vec3 normal{};
  glm::vec2 tex_coord{};
};

struct Material {
  GLuint albedo{};
  GLuint roughness{};
  GLuint normal{};
};

struct Model {
  GLuint vao{};
  GLuint vbo{};
  GLuint ebo{};
  GLsizei index_count{};
  const Material* material0{};
  const Material* material1{};
};

struct cgltf_material;

class Renderer {
 public:
  Renderer() = default;
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  Renderer(Renderer&&) = delete;
  Renderer& operator=(Renderer&&) = delete;

  struct ShaderPath {
    std::filesystem::path vert;
    std::filesystem::path frag;
  };

  bool load_shader(std::string name, const ShaderPath& path);
  // Loads a program from "//@stage <tag>" sections of a single .glsl file
  // (see resources/shaders/pbr_shaders.glsl).
  bool load_shader_sections(std::string name, const std::filesystem::path& file,
                            std::string_view vert_tag, std::string_view frag_tag);
  void install_shader(std::string_view name);
  [[nodiscard]] GLuint current_shader() const { return current_shader_; }
  // clang-format off
  using UniformValue = std::variant<float, glm::vec2, glm::vec3, glm::vec4, int, glm::mat3, glm::mat4>;
  void set_shader_uniform(std::string_view name, UniformValue value);
  void set_shader_uniform_array(std::string_view name, const glm::vec4* values, int count);
  // clang-format on

  bool load_model(std::string name, const std::filesystem::path& path);
  void draw_model(std::string_view name, const Transform& transform,
                  bool use_alternative_material = false);
  // Same as draw_model but with an explicit model matrix (used for the large
  // table plane and mirrored reflection draws).
  void draw_model_matrix(std::string_view name, const glm::mat4& model_mat,
                         bool use_alternative_material = false);

  // Projection/view used by subsequent draws (camera, light or mirror).
  void set_frame(const glm::mat4& projection, const glm::mat4& view) {
    projection_ = projection;
    view_ = view;
  }

  static void begin_outlining();
  static void end_outlining();
  void draw_model_outline(std::string_view name, const Transform& transform,
                          float thickness, const glm::vec4& color);

  static glm::mat4 make_model_matrix(const Transform& transform);

  // Loads (or returns the cached) 2D texture with mipmaps; 0 on failure.
  GLuint load_texture(const std::filesystem::path& path);

 private:
  void unload_shader(std::string_view name);
  bool link_program(std::string name, const std::string& vert_code,
                    const std::string& frag_code, const std::string& label);
  GLint uniform_location(std::string_view name);

  void unload_texture(const std::filesystem::path& path);

  const Material* load_material(const std::filesystem::path& parent,
                                const cgltf_material* cgltf_material);

  void unload_model(std::string_view name);

  GLuint current_shader_{};
  StringMap<GLuint> shader_map_;
  std::unordered_map<GLuint, StringMap<GLint>> uniform_cache_;

  // Keyed by path string: std::hash<std::filesystem::path> is missing from
  // some Apple libc++ releases.
  std::unordered_map<std::string, GLuint> texture_map_;
  float max_anisotropy_{};

  StringMap<Material> material_map_;
  StringMap<Model> model_map_;

  glm::mat4 projection_{1.0F};
  glm::mat4 view_{1.0F};
};
