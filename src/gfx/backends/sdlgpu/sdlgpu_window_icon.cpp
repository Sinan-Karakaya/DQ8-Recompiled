#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#if !defined(_WIN32) && !defined(__APPLE__)
// res/icon/dq8-window.bmp, embedded by dq8_embed_window_icon (cmake/Dq8Icon.cmake).
extern const unsigned char kWindowIconBmp[];
extern const unsigned int kWindowIconBmp_size;
#endif

namespace dq8::gfx {

void setWindowIcon(SDL_Window *window) {
#if !defined(_WIN32) && !defined(__APPLE__)
    SDL_Surface *icon = SDL_LoadBMP_IO(SDL_IOFromConstMem(kWindowIconBmp, kWindowIconBmp_size), true);
    if (!icon)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
#else
    (void)window;
#endif
}

} // namespace dq8::gfx
