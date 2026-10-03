// main.cpp — Chess Assist entry point.
//
//   chess-assist                       windowed app (side prompt on launch)
//   chess-assist --side black          skip the prompt
//   chess-assist --headless            no window: engine + automation API only
//   chess-assist --port 8765           automation console port (0 disables)
//   chess-assist --stockfish /path     explicit engine binary
//   chess-assist --movetime 1500       engine think time per position (ms)
//   chess-assist --moves "d4 e6"       play moves on startup
//   chess-assist --screenshot out.png  render ~90 frames, save PNG, exit
//   chess-assist --size 1280x800       window size

#include "game.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace {

void print_usage() {
  std::puts(
      "Chess Assist - 3D chess board with a live Stockfish assistant\n\n"
      "  --side white|black   preselect your side\n"
      "  --headless           run without a window (automation / CI)\n"
      "  --port N             automation console port (default 8765, 0 = off)\n"
      "  --stockfish PATH     Stockfish executable (default: auto-detect)\n"
      "  --movetime MS        engine time per position (default 1500)\n"
      "  --moves \"d4 e6\"      moves to play on startup\n"
      "  --screenshot FILE    save a PNG after N frames and exit\n"
      "  --frames N           frames to render before --screenshot (default 90)\n"
      "  --size WxH           window size (default 1440x900)\n"
      "  --scene gallery|studio  environment (default gallery)\n"
      "  --pitch DEG          initial camera pitch (12-88, default 54)\n"
      "  --perf               show the GPU profiler overlay (toggle with F3)\n"
      "  --demo               play the scripted showcase (used for the promo)\n"
      "  --record FILE        write raw RGBA frames at a fixed timestep (pipe to ffmpeg)\n"
      "  --fps N / --duration S  recording rate and length (default 30 / 15)\n"
      "  --ui-scale S         HUD scale, e.g. 2 for 4K captures\n"
      "  --version            print the version and exit\n"
      "  --gl-debug           check glGetError after every GL call\n");
}

std::string find_dir(const char* name, const char* compiled_default,
                     const std::filesystem::path& exe_dir) {
  namespace fs = std::filesystem;
  for (const fs::path& c : {fs::current_path() / name, exe_dir / name,
                            exe_dir / ".." / name, fs::path{compiled_default}}) {
    std::error_code ec;
    if (fs::is_directory(c, ec)) {
      return fs::weakly_canonical(c, ec).string();
    }
  }
  return compiled_default;
}

void error_callback(int /*code*/, const char* description) {
  LOG("GLFW", description);
}

GLFWwindow* create_window(bool gl_debug, int width, int height) {
  glfwSetErrorCallback(error_callback);
  if (glfwInit() != GLFW_TRUE) {
    LOG("GLFW", "Failed to initialize (no display? try --headless)");
    return nullptr;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GL_TRUE);
#endif
  // MSAA happens in our own offscreen target; the backbuffer stays simple.
  glfwWindowHint(GLFW_SAMPLES, 0);

  GLFWwindow* window{glfwCreateWindow(width, height, "Chess Assist", nullptr, nullptr)};
  if (window == nullptr) {
    LOG("GLFW", "Failed to create an OpenGL 3.3 core window");
    glfwTerminate();
    return nullptr;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  if (gladLoadGL(glfwGetProcAddress) == 0) {
    LOG("GL", "Failed to load OpenGL functions");
    glfwDestroyWindow(window);
    glfwTerminate();
    return nullptr;
  }
  if (!gl_debug) {
    gladUninstallGLDebug();  // skip per-call glGetError in normal runs
  }
  LOGF("GL", "{} | {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
       reinterpret_cast<const char*>(glGetString(GL_VERSION)));
  return window;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);  // broken engine pipe / HTTP client
#endif
  AppOptions options;
  int width{static_cast<int>(k_window_size.x)};
  int height{static_cast<int>(k_window_size.y)};
  for (int i = 1; i < argc; ++i) {
    const std::string a{argv[i]};
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--headless") {
      options.headless = true;
    } else if (a == "--port") {
      options.port = std::stoi(next());
    } else if (a == "--stockfish") {
      options.stockfish = next();
    } else if (a == "--movetime") {
      options.movetime_ms = std::stoi(next());
    } else if (a == "--side") {
      const std::string s{next()};
      options.side = s == "black" ? PieceColor::Black : PieceColor::White;
    } else if (a == "--moves") {
      std::istringstream in{next()};
      std::string m;
      while (in >> m) {
        // Skip move numbers like "1." in "1. d4 e6".
        if (m.find_first_not_of("0123456789.") != std::string::npos) {
          options.moves.push_back(m);
        }
      }
    } else if (a == "--screenshot") {
      options.screenshot_out = next();
    } else if (a == "--frames") {
      options.screenshot_frames = std::stoi(next());
    } else if (a == "--demo") {
      options.demo = true;
    } else if (a == "--record") {
      options.record_path = next();
    } else if (a == "--fps") {
      options.record_fps = std::stoi(next());
    } else if (a == "--duration") {
      options.record_seconds = std::stof(next());
    } else if (a == "--ui-scale") {
      options.ui_scale = std::stof(next());
    } else if (a == "--perf") {
      options.show_perf = true;
    } else if (a == "--version" || a == "-v") {
      std::printf("chess-assist %s\n", CHESS_ASSIST_VERSION);
      return 0;
    } else if (a == "--pitch") {
      options.camera_pitch = std::stof(next());
    } else if (a == "--scene") {
      const std::string v{next()};
      options.scene = v == "studio" ? SceneStyle::Studio : SceneStyle::Gallery;
    } else if (a == "--size") {
      const std::string v{next()};
      const auto x{v.find('x')};
      if (x != std::string::npos) {
        width = std::stoi(v.substr(0, x));
        height = std::stoi(v.substr(x + 1));
      }
    } else if (a == "--gl-debug") {
      options.gl_debug = true;
    } else if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "Unknown option %s\n\n", a.c_str());
      print_usage();
      return 2;
    }
  }

  const std::filesystem::path exe_dir{
      std::filesystem::absolute(argv[0]).parent_path()};
  options.resource_dir = find_dir("resources", CHESS_ASSIST_RESOURCE_DIR, exe_dir);
  options.web_dir = find_dir("web", CHESS_ASSIST_WEB_DIR, exe_dir);

  GLFWwindow* window{};
  if (!options.headless) {
    window = create_window(options.gl_debug, width, height);
    if (window == nullptr) {
      return 1;
    }
  }

  int rc{};
  {
    Game game{window, options};
    rc = game.run();
  }

  if (window != nullptr) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return rc;
}
