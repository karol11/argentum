#ifndef _AG_SDL_BINDINGS_H_
#define _AG_SDL_BINDINGS_H_

#include "../runtime/runtime.h"
#include "runtime/blob.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t ag_fn_sdlFfi_sdlInit         (int64_t flags);
void    ag_fn_sdlFfi_sdlQuit         ();

int64_t ag_fn_sdlFfi_createWindow    (AgString* title, int64_t x, int64_t y, int64_t w, int64_t h, int64_t flags);
void    ag_fn_sdlFfi_destroyWindow   (int64_t windowId);

int64_t ag_fn_sdlFfi_createRenderer  (int64_t windowId, int64_t index, int64_t flags);
void    ag_fn_sdlFfi_destroyRenderer (int64_t rendererId);

void    ag_fn_sdlFfi_waitEvent       (AgBlob* event);
void    ag_fn_sdlFfi_pollEvent       (AgBlob* event);
void    ag_fn_sdlFfi_delay           (int64_t millisec);

void    ag_fn_sdlFfi_setRendererDrawColor (int64_t rendererId, int64_t r, int64_t g, int64_t b, int64_t a);
void    ag_fn_sdlFfi_rendererClear        (int64_t rendererId);
void    ag_fn_sdlFfi_rendererFillRect     (int64_t rendererId, int64_t x, int64_t y, int64_t w, int64_t h);
void    ag_fn_sdlFfi_blt                  (int64_t rendererId, int64_t textureId, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh);
void    ag_fn_sdlFfi_rendererPresent      (int64_t rendererId);

int64_t ag_fn_sdlFfi_createTextureFromSurface (int64_t rendererId, int64_t surfaceId);
void    ag_fn_sdlFfi_destroyTexture           (int64_t textureId);
void    ag_fn_sdlFfi_setTextureAlphaMod       (int64_t texId, int64_t multiplier);
void    ag_fn_sdlFfi_setTextureColorMod       (int64_t textureId, int64_t color);

int64_t ag_fn_sdlFfi_imgInit (int64_t flags);
void    ag_fn_sdlFfi_imgQuit ();

int64_t ag_fn_sdlFfi_imgLoad        (AgString* file_name);
void    ag_fn_sdlFfi_freeSurface (int64_t surfaceId);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // _AG_SDL_BINDINGS_H_
