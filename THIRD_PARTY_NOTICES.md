# Third-party notices

Chess Assist bundles or downloads the following components. Each keeps its
own license; the full texts are in the files referenced below.

| Component | Version | License | Location |
|---|---|---|---|
| [chess-3d](https://github.com/ecyk/chess-3d) (base renderer, rules engine) | 2023 | MIT, © 2023 Enes Yerlikaya | `LICENSE` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | MIT, © 2014-2026 Omar Cornut | `external/imgui/LICENSE.txt` |
| [glad](https://github.com/Dav1dde/glad) (GL 3.3 core loader) | 2 | (WTFPL OR CC0-1.0) AND Apache-2.0 (Khronos specs) | `external/include/glad/gl.h` |
| [stb_image / stb_image_write](https://github.com/nothings/stb) | 2.27 / 1.16 | Public domain or MIT | `external/include/stb_*.h` |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.x | MIT | `external/include/cgltf.h` |
| [GLFW](https://www.glfw.org) | 3.3+ (system or fetched) | zlib/libpng | not vendored |
| [GLM](https://github.com/g-truc/glm) | 1.0 (system or fetched) | MIT | not vendored |
| Chess set models & textures ([Poly Haven](https://polyhaven.com/a/chess_set)) | — | CC0 | `resources/models/` |
| Roboto Medium, Cousine fonts (via Dear ImGui `misc/fonts`) | — | Apache-2.0 | `resources/fonts/` |
| Gallery hall backdrop photo | — | Provided by the project author | `resources/textures/gallery_hall.jpg` |

[Stockfish](https://stockfishchess.org) (GPL-3.0) is **not** bundled or linked.
Chess Assist starts a separately installed Stockfish binary as a child process
and talks to it over the UCI text protocol.

Test tooling: [Playwright](https://playwright.dev) (Apache-2.0), installed via npm.
