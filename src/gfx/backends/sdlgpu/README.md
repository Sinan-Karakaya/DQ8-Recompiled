# SDL GPU renderer

Implements the GS backend with SDL3 GPU. The game selects it with `--gs=sdlgpu`;
`--scale=N` controls internal resolution.

| File | Responsibility |
| --- | --- |
| `sdlgpu_backend` | Draw dispatch, pipelines, and render-pass management |
| `sdlgpu_device` | SDL device, window, and GPU resource lifetime |
| `sdlgpu_targets` | Render targets, page ownership, feedback, and readback |
| `sdlgpu_textures` | Texture decoding, caching, and invalidation |
| `sdlgpu_display` | Display composition, and how the picture is fitted into the window |
| `sdlgpu_window` | Display options and the overlay hook the in-game menu (`src/ui`) draws through |
| `sdlgpu_input` | Keyboard and gamepad input, with remappable bindings and controller selection |
| `shaders` | GS drawing, reinterpretation, 8-bit texture expansion, and display shaders |

See the [graphics guide](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Graphics)
for synchronization rules and limitations, and the
[testing guide](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing)
for the standalone renderer harness.
