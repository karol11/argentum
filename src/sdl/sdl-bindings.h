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

bool ag_m_sdl_Sdl_sdl_init(AgSdl* thiz, int64_t sdl_flags, int64_t img_flags);
AgBlob* ag_m_sdl_Sdl_sdl_pollEvent(AgSdl* thiz);
void ag_fn_sdl_disposeSdl(AgSdl* thiz);

bool ag_m_sdl_Window_sdl_init(AgSdlWindow* thiz, AgString* title, AgSdlRect* bounds, int64_t flags, int64_t r_flags);
void ag_fn_sdl_disposeWindow(AgSdlWindow* thiz);
void ag_m_sdl_Window_sdl_fill(AgSdlWindow* thiz, int64_t color);
void ag_m_sdl_Window_sdl_fillRectXYWH(AgSdlWindow* thiz, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);
void ag_m_sdl_Window_sdl_fillRect(AgSdlWindow* thiz, AgSdlRect* r, int64_t color);
void ag_m_sdl_Window_sdl_bltXYWH(AgSdlWindow* thiz, AgSdlTexture* tex, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh);
void ag_m_sdl_Window_sdl_blt(AgSdlWindow* thiz, AgSdlTexture* tex, AgSdlRect* s, AgSdlRect* d);
void ag_m_sdl_Window_sdl_flip(AgSdlWindow* thiz);

bool ag_m_sdl_Texture_sdl_load(AgSdlTexture* tex, AgSdlWindow* w, AgString* file_name);
void ag_fn_sdl_disposeTexture(AgSdlTexture* tex);
void ag_m_sdl_Texture_sdl_setAlphaMod(AgSdlTexture* tex, int64_t multiplier);
void ag_m_sdl_Texture_sdl_setColorMod(AgSdlTexture* tex, int64_t color);

bool ag_m_sdl_Font_sdl_load(AgSdlFont* thiz, AgString* fontName, int style);
void ag_fn_sdl_disposeFont(AgSdlFont* fnt);
AgString* ag_m_sdl_Font_sdl_name(AgSdlFont* thiz);
AgString* ag_m_sdl_Font_sdl_style(AgSdlFont* thiz);
int64_t ag_m_sdl_Font_sdl_stylesCount(AgSdlFont* thiz);
void ag_m_sdl_Font_sdl_renderTo(AgSdlFont* thiz, AgSdlTexture* r, AgString* str, int ptSize, int flags, AgSdlWindow* wnd);
int64_t ag_m_sdl_Font_sdl_fit(AgSdlFont* thiz, AgString* s, int64_t ptSize, int64_t flags, int64_t width);
int64_t ag_m_sdl_Font_sdl_measure(AgSdlFont* thiz, AgString* str, int64_t ptSize, int64_t flags, AgSdlRect* extents);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // _AG_SDL_BINDINGS_H_
