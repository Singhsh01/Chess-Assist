#pragma once
// GpuProfiler.h — per-pass GPU timings with GL_TIME_ELAPSED queries.
//
// Queries are double-buffered: results are read one frame later, and only
// when GL reports them available, so profiling never stalls the pipeline.
// Values are smoothed with an exponential moving average for display.

#include <glad/gl.h>

#include <array>
#include <string_view>

class GpuProfiler {
 public:
  enum Pass : int { Picking, Shadow, Reflection, Scene, Post, Hud, Count };

  static constexpr std::array<std::string_view, Count> k_names{
      "picking", "shadow", "reflection", "scene", "post", "hud"};

  GpuProfiler() = default;
  ~GpuProfiler() {
    if (initialized_) {
      glDeleteQueries(static_cast<GLsizei>(queries_.size() * Count), queries_[0].data());
    }
  }
  GpuProfiler(const GpuProfiler&) = delete;
  GpuProfiler& operator=(const GpuProfiler&) = delete;
  GpuProfiler(GpuProfiler&&) = delete;
  GpuProfiler& operator=(GpuProfiler&&) = delete;

  void init() {
    for (auto& set : queries_) {
      glGenQueries(Count, set.data());
    }
    initialized_ = true;
  }

  void begin(Pass pass) {
    if (initialized_ && active_ < 0) {
      glBeginQuery(GL_TIME_ELAPSED, queries_[frame_][pass]);
      active_ = pass;
    }
  }

  void end() {
    if (active_ >= 0) {
      glEndQuery(GL_TIME_ELAPSED);
      issued_[frame_][active_] = true;
      active_ = -1;
    }
  }

  // Call once per frame after all passes: collects last frame's results.
  void end_frame() {
    if (!initialized_) {
      return;
    }
    const int prev{1 - frame_};
    for (int p = 0; p < Count; ++p) {
      if (!issued_[prev][p]) {
        continue;
      }
      GLint available{};
      glGetQueryObjectiv(queries_[prev][p], GL_QUERY_RESULT_AVAILABLE, &available);
      if (available == 0) {
        continue;
      }
      GLuint64 ns{};
      glGetQueryObjectui64v(queries_[prev][p], GL_QUERY_RESULT, &ns);
      const float ms{static_cast<float>(ns) * 1e-6F};
      ms_[p] = ms_[p] == 0.0F ? ms : ms_[p] * 0.92F + ms * 0.08F;
      issued_[prev][p] = false;
    }
    frame_ = prev;
  }

  [[nodiscard]] float ms(int pass) const { return ms_[pass]; }
  [[nodiscard]] float total_ms() const {
    float t{};
    for (const float v : ms_) {
      t += v;
    }
    return t;
  }

 private:
  bool initialized_{};
  int frame_{};
  int active_{-1};
  std::array<std::array<GLuint, Count>, 2> queries_{};
  std::array<std::array<bool, Count>, 2> issued_{};
  std::array<float, Count> ms_{};
};
