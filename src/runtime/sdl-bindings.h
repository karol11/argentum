#ifndef _AG_SDL_BINDINGS_H_
#define _AG_SDL_BINDINGS_H_

#include "../runtime/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t ag_fn_sys_sdl_init            (int64_t flags);
void    ag_fn_sys_sdl_quit            ();

int64_t ag_fn_sys_sdl_createWindow    (AgString* title, int64_t x, int64_t y, int64_t w, int64_t h, int64_t flags);
void    ag_fn_sys_sdl_destroyWindow   (int64_t windowId);

int64_t ag_fn_sys_sdl_createRenderer  (int64_t windowId, int64_t index, int64_t flags);
void    ag_fn_sys_sdl_destroyRenderer (int64_t rendererId);

void    ag_fn_sys_sdl_waitEvent       (AgBlob* event);
void    ag_fn_sys_sdl_pollEvent       (AgBlob* event);
void    ag_fn_sys_sdl_delay           (int64_t millisec);

void    ag_fn_sys_sdl_setRendererDrawColor (int64_t rendererId, int64_t r, int64_t g, int64_t b, int64_t a);
void    ag_fn_sys_sdl_rendererClear        (int64_t rendererId);
void    ag_fn_sys_sdl_rendererFillRect     (int64_t rendererId, int64_t x, int64_t y, int64_t w, int64_t h);
void    ag_fn_sys_sdl_blt                  (int64_t rendererId, int64_t textureId, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh);
void    ag_fn_sys_sdl_rendererPresent      (int64_t rendererId);

int64_t ag_fn_sys_sdl_createTextureFromSurface (int64_t rendererId, int64_t surfaceId);
void    ag_fn_sys_sdl_destroyTexture           (int64_t textureId);
void    ag_fn_sys_sdl_setTextureAlphaMod       (int64_t texId, int64_t multiplier);
void    ag_fn_sys_sdl_setTextureColorMod       (int64_t textureId, int64_t color);

int64_t ag_fn_sys_img_init (int64_t flags);
void    ag_fn_sys_img_quit ();

int64_t ag_fn_sys_img_load        (AgString* file_name);
void    ag_fn_sys_sdl_freeSurface (int64_t surfaceId);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // _AG_SDL_BINDINGS_H_
