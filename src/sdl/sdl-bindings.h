#ifndef _AG_SDL_BINDINGS_H_
#define _AG_SDL_BINDINGS_H_

#include "runtime.h"
#include "blob.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AgSdl AgSdl;
typedef struct AgSdlWindow AgSdlWindow;
typedef struct AgSdlTexture AgSdlTexture;
typedef struct AgSdlFont AgSdlFont;
typedef struct AgSdlRect AgSdlRect;

bool          ag_m_sdl_Sdl_sdl_init      (AgSdl* thiz, int32_t sdl_flags, int32_t img_flags);
AgBlob*       ag_m_sdl_Sdl_sdl_pollEvent (AgSdl* thiz);
void          ag_fn_sdl_disposeSdl       (AgSdl* thiz);

bool          ag_m_sdl_Window_sdl_init         (AgSdlWindow* thiz, AgString* title, AgSdlRect* bounds, int32_t flags, int32_t r_flags);
AgSdlFont*    ag_m_sdl_Window_sdl_loadFont     (AgSdlWindow* thiz, AgString* fileName, int32_t style); // returns ?*Font
AgSdlTexture* ag_m_sdl_Window_sdl_loadTexture  (AgSdlWindow* thiz, AgString* fileName); // ?*Texture
void          ag_m_sdl_Window_sdl_fill         (AgSdlWindow* thiz, int32_t color);
void          ag_m_sdl_Window_sdl_fillRectXYWH (AgSdlWindow* thiz, int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
void          ag_m_sdl_Window_sdl_fillRect     (AgSdlWindow* thiz, AgSdlRect* r, int32_t color);
void          ag_m_sdl_Window_sdl_bltXYWH      (AgSdlWindow* thiz, AgSdlTexture* tex, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, int32_t dw, int32_t dh, int32_t color);
void          ag_m_sdl_Window_sdl_blt          (AgSdlWindow* thiz, AgSdlTexture* tex, AgSdlRect* s, AgSdlRect* d, int32_t color);
void          ag_m_sdl_Window_sdl_flip         (AgSdlWindow* thiz);
void          ag_fn_sdl_disposeWindow          (AgSdlWindow* thiz);

void ag_fn_sdl_disposeTexture(AgSdlTexture* tex);

AgString*     ag_m_sdl_Font_sdl_name        (AgSdlFont* thiz);
AgString*     ag_m_sdl_Font_sdl_style       (AgSdlFont* thiz);
int32_t       ag_m_sdl_Font_sdl_stylesCount (AgSdlFont* thiz);
AgSdlTexture* ag_m_sdl_Font_sdl_render      (AgSdlFont* thiz, AgString* str, int32_t ptSize, int32_t flags); // returns *Texture
int32_t       ag_m_sdl_Font_sdl_fit         (AgSdlFont* thiz, AgString* s, int32_t ptSize, int32_t flags, int32_t width);
int32_t       ag_m_sdl_Font_sdl_measure     (AgSdlFont* thiz, AgString* str, int32_t ptSize, int32_t flags, AgSdlRect* extents);
void          ag_fn_sdl_disposeFont         (AgSdlFont* fnt);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // _AG_SDL_BINDINGS_H_
