#pragma once

#include "common.hpp"

// Orbit camera around the board centre. Yaw 0° looks from White's side
// (White at the bottom of the screen), yaw 180° from Black's side.
class Camera {
 public:
  static constexpr float k_fov_degrees{42.0F};
  static constexpr float k_near{1.0F};
  static constexpr float k_far{420.0F};

  void set_orbit(float yaw_degrees, float pitch_degrees, float distance,
                 const glm::vec3& target = {0.0F, 0.0F, 0.0F});

  [[nodiscard]] glm::mat4 calculate_view_matrix() const;
  [[nodiscard]] glm::mat4 calculate_projection_matrix(float aspect) const;

  [[nodiscard]] const glm::vec3& get_position() const { return position_; }
  [[nodiscard]] const glm::vec3& get_target() const { return target_; }
  [[nodiscard]] float get_distance() const { return distance_; }

 private:
  glm::vec3 position_{0.0F, 40.0F, -40.0F};
  glm::vec3 target_{};
  float distance_{56.0F};
};
