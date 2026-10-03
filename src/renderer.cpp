#include "renderer.hpp"

#include <cgltf.h>
#include <stb_image.h>

#include <algorithm>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <sstream>

Renderer::~Renderer() {
  // Plain loops (not std::views::keys) for older Apple libc++ versions.
  for (const auto& entry : model_map_) {
    unload_model(entry.first);
  }
  for (const auto& entry : texture_map_) {
    unload_texture(entry.first);
  }
  for (const auto& entry : shader_map_) {
    unload_shader(entry.first);
  }
}

namespace {
std::string read_file(const std::filesystem::path& path) {
  std::ifstream file{path};
  using istreambuf_iter = std::istreambuf_iterator<char>;
  std::string text{istreambuf_iter{file}, istreambuf_iter{}};
  if (!file) {
    LOGF("GL", "Failed to read \"{}\"", path.string());
    return {};
  }
  return text;
}

GLuint gl_create_shader(const char* code, GLenum type, std::string& buffer) {
  const GLuint shader{glCreateShader(type)};
  glShaderSource(shader, 1, &code, nullptr);
  glCompileShader(shader);

  GLint success{};
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (success == 0) {
    const auto size{static_cast<GLsizei>(buffer.size())};
    glGetShaderInfoLog(shader, size, nullptr, buffer.data());
    LOG("GL", buffer);
  }

  return shader;
}

struct Shader {
  GLuint vert;
  GLuint frag;
};

GLuint gl_create_program(Shader shader, std::string& buffer) {
  const GLuint program{glCreateProgram()};

  glAttachShader(program, shader.vert);
  glAttachShader(program, shader.frag);
  glLinkProgram(program);

  glDeleteShader(shader.vert);
  glDeleteShader(shader.frag);

  GLint success{};
  glGetProgramiv(program, GL_LINK_STATUS, &success);
  if (success == 0) {
    const auto size{static_cast<GLsizei>(buffer.size())};
    glGetProgramInfoLog(program, size, nullptr, buffer.data());
    glDeleteProgram(program);
    LOG("GL", buffer);
    return 0;
  }

  return program;
}

struct Texture {
  unsigned char* data{};
  int width{};
  int height{};
  int components{};
};

Texture load_texture(const std::filesystem::path& path) {
  int width{};
  int height{};
  int components{};
  unsigned char* data{
      stbi_load(path.string().c_str(), &width, &height, &components, 0)};
  if (data == nullptr) {
    LOGF("GL", "Failed to load \"{}\"", path.string());
    return {};
  }
  return {data, width, height, components};
}

GLuint gl_create_texture(const Texture& texture) {
  GLuint id{};
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);

  GLint format{};
  switch (texture.components) {
    case 1:
      format = GL_RED;
      break;
    case 2:
      format = GL_RG;
      break;
    case 3:
      format = GL_RGB;
      break;
    case 4:
      format = GL_RGBA;
      break;
    default:
      assert(false);
      break;
  }

  glTexImage2D(GL_TEXTURE_2D, 0, format, texture.width, texture.height, 0,
               format, GL_UNSIGNED_BYTE, texture.data);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);

  stbi_image_free(texture.data);
  return id;
}
}  // namespace

bool Renderer::link_program(std::string name, const std::string& vert_code,
                            const std::string& frag_code,
                            const std::string& label) {
  assert(!shader_map_.contains(name));
  std::string buffer;
  buffer.resize(4096);
  // clang-format off
  const GLuint vert_shader{gl_create_shader(vert_code.c_str(), GL_VERTEX_SHADER, buffer)};
  const GLuint frag_shader{gl_create_shader(frag_code.c_str(), GL_FRAGMENT_SHADER, buffer)};
  const GLuint program{gl_create_program({vert_shader, frag_shader}, buffer)};
  // clang-format on
  if (program == 0) {
    LOGF("GL", "Failed to link shader '{}' ({})", name, label);
    return false;
  }
  LOGF("GL", "Shader '{}' loaded ({}) (id: {})", name, label, program);
  shader_map_.insert_or_assign(std::move(name), program);
  return true;
}

bool Renderer::load_shader(std::string name, const ShaderPath& path) {
  return link_program(std::move(name), read_file(path.vert),
                      read_file(path.frag),
                      path.vert.filename().string() + " + " +
                          path.frag.filename().string());
}

bool Renderer::load_shader_sections(std::string name,
                                    const std::filesystem::path& file,
                                    std::string_view vert_tag,
                                    std::string_view frag_tag) {
  // Sections start with a line "//@stage <tag>" and run until the next one.
  const std::string text{read_file(file)};
  StringMap<std::string> sections;
  std::istringstream in{text};
  std::string line;
  std::string current;
  while (std::getline(in, line)) {
    if (line.rfind("//@stage ", 0) == 0) {
      current = line.substr(9);
      while (!current.empty() && (current.back() == '\r' || current.back() == ' ')) {
        current.pop_back();
      }
      continue;
    }
    if (!current.empty()) {
      sections[current] += line + "\n";
    }
  }
  const auto vs{sections.find(vert_tag)};
  const auto fs{sections.find(frag_tag)};
  if (vs == sections.end() || fs == sections.end()) {
    LOGF("GL", "Missing stage '{}' or '{}' in {}", vert_tag, frag_tag,
         file.string());
    return false;
  }
  return link_program(std::move(name), vs->second, fs->second,
                      file.filename().string() + ":" + std::string{vert_tag} +
                          "+" + std::string{frag_tag});
}

void Renderer::unload_shader(std::string_view name) {
  const auto it{shader_map_.find(name)};
  assert(it != shader_map_.end());
  const GLuint id{it->second};
  glDeleteProgram(id);
}

void Renderer::install_shader(std::string_view name) {
  const auto it{shader_map_.find(name)};
  assert(it != shader_map_.end());
  if (const GLuint id = it->second; current_shader_ != id) {
    glUseProgram(id);
    current_shader_ = id;
  }
}

GLint Renderer::uniform_location(std::string_view name) {
  auto& cache{uniform_cache_[current_shader_]};
  if (const auto it = cache.find(name); it != cache.end()) {
    return it->second;
  }
  const std::string key{name};
  const GLint location{glGetUniformLocation(current_shader_, key.c_str())};
  cache.emplace(key, location);
  return location;
}

void Renderer::set_shader_uniform(std::string_view name, UniformValue value) {
  const GLint location{uniform_location(name)};
  if (location < 0) {
    return;
  }
  // clang-format off
  std::visit(
      overload{
          [location](float value) { glUniform1fv(location, 1, &value); },
          [location](glm::vec2 value) { glUniform2fv(location, 1, value_ptr(value)); },
          [location](const glm::vec3& value) { glUniform3fv(location, 1, value_ptr(value)); },
          [location](const glm::vec4& value) { glUniform4fv(location, 1, value_ptr(value)); },
          [location](int value) { glUniform1iv(location, 1, &value); },
          [location](const glm::mat3& value) { glUniformMatrix3fv(location, 1, GL_FALSE, value_ptr(value)); },
          [location](const glm::mat4& value) { glUniformMatrix4fv(location, 1, GL_FALSE, value_ptr(value)); }},
      value);
  // clang-format on
}

void Renderer::set_shader_uniform_array(std::string_view name,
                                        const glm::vec4* values, int count) {
  if (const GLint location = uniform_location(name); location >= 0 && count > 0) {
    glUniform4fv(location, count, value_ptr(values[0]));
  }
}

GLuint Renderer::load_texture(const std::filesystem::path& path) {
  if (const auto it = texture_map_.find(path.string()); it != texture_map_.end()) {
    return it->second;
  }
  const Texture texture{::load_texture(path)};
  if (texture.data == nullptr) {
    return 0;
  }
  const GLuint id{gl_create_texture(texture)};
  // Anisotropic filtering keeps the board grain crisp at grazing angles.
  constexpr GLenum k_max_anisotropy{0x84FF};  // GL_MAX_TEXTURE_MAX_ANISOTROPY
  constexpr GLenum k_anisotropy{0x84FE};      // GL_TEXTURE_MAX_ANISOTROPY
  if (max_anisotropy_ == 0.0F) {
    max_anisotropy_ = -1.0F;
    GLint count{};
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
      const auto* ext{reinterpret_cast<const char*>(
          glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)))};
      if (ext != nullptr &&
          std::string_view{ext} == "GL_EXT_texture_filter_anisotropic") {
        glGetFloatv(k_max_anisotropy, &max_anisotropy_);
        break;
      }
    }
  }
  if (max_anisotropy_ > 1.0F) {
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameterf(GL_TEXTURE_2D, k_anisotropy, std::min(max_anisotropy_, 8.0F));
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  texture_map_.insert_or_assign(path.string(), id);
  LOGF("GL", "Texture loaded (file: \"{}\") (id: {})", path.string(), id);
  return id;
}

void Renderer::unload_texture(const std::filesystem::path& path) {
  const auto it{texture_map_.find(path.string())};
  assert(it != texture_map_.end());
  const GLuint id{it->second};
  glDeleteTextures(1, &id);
  LOGF("GL", "Texture deleted (file: \"{}\") (id: {})", path.string(), id);
}

const Material* Renderer::load_material(const std::filesystem::path& parent,
                                        const cgltf_material* cgltf_material) {
  if (const auto it = material_map_.find(cgltf_material->name);
      it != material_map_.end()) {
    return &it->second;
  }

  auto get_texture = [this, &parent](const char* uri) {
    std::filesystem::path path{parent};
    path += '/';
    path += uri;
    return load_texture(path);
  };

  GLuint albedo{};
  GLuint roughness{};
  GLuint normal{};
  const auto& pbr = cgltf_material->pbr_metallic_roughness;
  if (pbr.base_color_texture.texture != nullptr) {
    albedo = get_texture(pbr.base_color_texture.texture->image->uri);
  }
  if (pbr.metallic_roughness_texture.texture != nullptr) {
    roughness = get_texture(pbr.metallic_roughness_texture.texture->image->uri);
  }
  if (cgltf_material->normal_texture.texture != nullptr) {
    normal = get_texture(cgltf_material->normal_texture.texture->image->uri);
  }
  auto it = material_map_.insert_or_assign(cgltf_material->name,
                                           Material{albedo, roughness, normal});
  return &it.first->second;
}

bool Renderer::load_model(std::string name, const std::filesystem::path& path) {
  assert(!model_map_.contains(name));

  constexpr cgltf_options options{};
  cgltf_data* data{};
  cgltf_result result{cgltf_parse_file(&options, path.string().c_str(), &data)};
  if (result != cgltf_result_success) {
    LOGF("GL", "Failed to load \"{}\"", path.string());
    return false;
  }

  result = cgltf_load_buffers(&options, data, path.string().c_str());
  if (result != cgltf_result_success) {
    LOGF("GL", "Failed to load buffers for \"{}\"", path.string());
    cgltf_free(data);
    return false;
  }

  result = cgltf_validate(data);
  if (result != cgltf_result_success) {
    LOGF("GL", "Invalid gltf file \"{}\"", path.string());
    cgltf_free(data);
    return false;
  }

  std::vector<Vertex> vertices;
  std::vector<unsigned int> indices;

  assert(data->scene->nodes_count == 1);
  const cgltf_node* node = data->scene->nodes[0];

  assert(node->children_count == 0);
  assert(node->mesh->primitives_count == 1);
  const cgltf_primitive* primitive = &node->mesh->primitives[0];

  const Material* material0{};
  if (primitive->material != nullptr) {
    material0 = load_material(path.parent_path(), primitive->material);
  }

  assert(primitive->mappings_count == 0 || primitive->mappings_count == 2);

  const Material* material1{};
  if (primitive->mappings_count == 2) {
    // clang-format off
    material0 = load_material(path.parent_path(), primitive->mappings[0].material);
    material1 = load_material(path.parent_path(), primitive->mappings[1].material);
    // clang-format on
  }

  const cgltf_accessor* position{};
  const cgltf_accessor* normal{};
  const cgltf_accessor* tex_coord{};
  for (size_t i = 0; i < primitive->attributes_count; i++) {
    const cgltf_attribute* attribute = &primitive->attributes[i];
    const cgltf_accessor* accessor = attribute->data;

    switch (attribute->type) {
      case cgltf_attribute_type_position:
        position = accessor;
        break;
      case cgltf_attribute_type_normal:
        normal = accessor;
        break;
      case cgltf_attribute_type_texcoord:
        tex_coord = accessor;
        break;
      default:
        assert(false);
        break;
    }
  }

  if (position == nullptr || normal == nullptr || tex_coord == nullptr) {
    LOGF("GL", "Model \"{}\" lacks position/normal/uv attributes", path.string());
    cgltf_free(data);
    return false;
  }

  size_t vertex_count{vertices.size()};
  vertices.resize(vertex_count + position->count);
  for (size_t i = 0; i < position->count; i++, vertex_count++) {
    // clang-format off
    cgltf_accessor_read_float(position, i, value_ptr(vertices[vertex_count].position), 3);
    cgltf_accessor_read_float(normal, i, value_ptr(vertices[vertex_count].normal), 3);
    cgltf_accessor_read_float(tex_coord, i, value_ptr(vertices[vertex_count].tex_coord), 2);
    // clang-format on
  }

  indices.resize(primitive->indices->count);
  for (cgltf_size i = 0; i < primitive->indices->count; i++) {
    indices[i] = static_cast<unsigned int>(
        cgltf_accessor_read_index(primitive->indices, i));
  }

  cgltf_free(data);

  GLuint vao{};
  GLuint vbo{};
  GLuint ebo{};

  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glGenBuffers(1, &ebo);

  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
               vertices.data(), GL_STATIC_DRAW);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)),
               indices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, position)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, normal)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<void*>(offsetof(Vertex, tex_coord)));

  glBindVertexArray(0);

  model_map_.insert_or_assign(
      std::move(name),
      Model{vao, vbo, ebo, static_cast<GLsizei>(indices.size()), material0,
            material1});
  LOGF("GL", "Model loaded (file: \"{}\")", path.string());
  return true;
}

void Renderer::unload_model(std::string_view name) {
  const auto it{model_map_.find(name)};
  assert(it != model_map_.end());
  const Model& model{it->second};
  glDeleteVertexArrays(1, &model.vao);
  glDeleteBuffers(1, &model.vbo);
  glDeleteBuffers(1, &model.ebo);
  LOGF("GL", "Model deleted (name: \"{}\") (vao: {})", name, model.vao);
}

glm::mat4 Renderer::make_model_matrix(const Transform& transform) {
  return scale(rotate(translate(glm::identity<glm::mat4>(), transform.position),
                      glm::radians(transform.rotation), {0.0F, 1.0F, 0.0F}),
               glm::vec3{transform.scale});
}

void Renderer::draw_model(std::string_view name, const Transform& transform,
                          bool use_alternative_material) {
  draw_model_matrix(name, make_model_matrix(transform), use_alternative_material);
}

void Renderer::draw_model_matrix(std::string_view name,
                                 const glm::mat4& model_mat,
                                 bool use_alternative_material) {
  const auto it{model_map_.find(name)};
  assert(it != model_map_.end());

  const Model& model{it->second};
  const Material* material{use_alternative_material && model.material1 != nullptr
                               ? model.material1
                               : model.material0};
  // Texture units 0-2 hold the material; 3+ are reserved for the pipeline
  // (shadow map, planar reflection). Always rebind: ImGui and the post passes
  // also touch unit 0, so a cache would go stale.
  if (material != nullptr) {
    if (material->albedo != 0) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, material->albedo);
      set_shader_uniform("albedo_tex", 0);
    }
    if (material->roughness != 0) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, material->roughness);
      set_shader_uniform("roughness_tex", 1);
    }
    if (material->normal != 0) {
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, material->normal);
      set_shader_uniform("normal_tex", 2);
    }
    set_shader_uniform("has_normal_map", material->normal != 0 ? 1 : 0);
  }

  set_shader_uniform("projection", projection_);
  set_shader_uniform("view", view_);
  set_shader_uniform("model", model_mat);
  set_shader_uniform("normal_mat", glm::mat3{transpose(inverse(glm::mat3(model_mat)))});

  glBindVertexArray(model.vao);
  glDrawElements(GL_TRIANGLES, model.index_count, GL_UNSIGNED_INT, nullptr);
  glBindVertexArray(0);
}

void Renderer::draw_model_outline(std::string_view name,
                                  const Transform& transform, float thickness,
                                  const glm::vec4& color) {
  glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
  glStencilMask(0x00);
  set_shader_uniform("outline_thickness", thickness);
  set_shader_uniform("color", color);
  draw_model(name, transform);
  glStencilMask(0xFF);
  glStencilFunc(GL_ALWAYS, 0, 0xFF);
}

void Renderer::begin_outlining() {
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilFunc(GL_ALWAYS, 1, 0xFF);
  glStencilMask(0xFF);
}

void Renderer::end_outlining() {
  glStencilMask(0xFF);
  glStencilFunc(GL_ALWAYS, 0, 0xFF);
}

