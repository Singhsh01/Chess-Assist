#include "camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

void Camera::set_orbit(float yaw_degrees, float pitch_degrees, float distance,
                       const glm::vec3& target) {
  const float yaw{glm::radians(yaw_degrees)};
  const float pitch{glm::radians(pitch_degrees)};
  // White's back rank sits at -Z, so yaw 0 places the camera on -Z looking
  // towards +Z: a-file on the left, rank 1 nearest the viewer.
  const glm::vec3 offset{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                         -std::cos(yaw) * std::cos(pitch)};
  target_ = target;
  distance_ = distance;
  position_ = target + offset * distance;
}

glm::mat4 Camera::calculate_view_matrix() const {
  return lookAt(position_, target_, {0.0F, 1.0F, 0.0F});
}

glm::mat4 Camera::calculate_projection_matrix(float aspect) const {
  return glm::perspective(glm::radians(k_fov_degrees), aspect, k_near, k_far);
}
