#include "../runtime/runtime.h"
#include "../runtime/blob.h"
#include "../runtime/sdl-bindings.h"
#include "utils/utf8.h"

#include "SDL.h"
#include "SDL_image.h"
#include "SDL_ttf.h"

typedef struct AgSdlRect {
    AgObject base;
    int64_t x, y, w, h;
} AgSdlRect;

typedef struct AgSdl {
    AgObject base;
    AgBlob* event;
} AgSdl;

typedef struct AgSdlWindow {
    AgObject base;
    SDL_Window* sdl_wnd;
    SDL_Renderer* sdl_rend;
} AgSdlWindow;

typedef struct AgSdlTexture {
    AgObject base;
    SDL_Texture* sdl_tex;
    int64_t w, h;
} AgSdlTexture;

typedef struct AgSdlFont {
    AgObject base;
    TTF_Font* sdl_font;
    int64_t last_pt_size;
    int64_t last_flags;
} AgSdlFont;

bool ag_m_sdl_Sdl_sdl_init(AgSdl* thiz, int64_t sdl_flags, int64_t img_flags) {
    ag_make_blob_fit(thiz->event, sizeof(SDL_Event));
    return
        SDL_Init((int)sdl_flags) == 0 &&
        IMG_Init((int)img_flags) == 0 &&
        TTF_Init() == 0;
}

AgBlob* ag_m_sdl_Sdl_sdl_pollEvent(AgSdl* thiz) {
    return SDL_PollEvent((SDL_Event*)thiz->event->bytes)
        ? ag_retain_pin(&thiz->event->head), thiz->event
        : NULL;
}

void ag_destroy_sdl_Sdl(AgSdl* thiz) {
    if (thiz->event->bytes_count) {
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
    }
}

bool ag_m_sdl_Window_sdl_init(AgSdlWindow* thiz, AgString* title, AgSdlRect* bounds, int64_t flags, int64_t r_flags) {
    thiz->sdl_wnd = SDL_CreateWindow(title->ptr, (int)bounds->x, (int)bounds->y, (int)bounds->w, (int)bounds->h, (int)flags);
    thiz->sdl_rend = SDL_CreateRenderer(thiz->sdl_wnd, 0, (int)r_flags);
    return thiz->sdl_wnd && thiz->sdl_rend;
}

void ag_destroy_sdl_Window(AgSdlWindow* thiz) {
    if (thiz->sdl_rend)
        SDL_DestroyRenderer(thiz->sdl_rend);
    if (thiz->sdl_wnd)
        SDL_DestroyWindow(thiz->sdl_wnd);
}

void ag_m_sdl_Window_sdl_fill(AgSdlWindow* thiz, int64_t color) {
    SDL_SetRenderDrawColor(thiz->sdl_rend, (int)color, (int)color >> 8, (int)color >> 16, (int)color >> 24);
    SDL_RenderClear(thiz->sdl_rend);
}

void ag_m_sdl_Window_sdl_fillRectXYWH(AgSdlWindow* thiz, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    SDL_SetRenderDrawColor(thiz->sdl_rend, (int)color, (int)color >> 8, (int)color >> 16, (int)color >> 24);
    SDL_RenderFillRect(thiz->sdl_rend,&(SDL_Rect){ (int)x, (int)y, (int)w, (int)h });
}

void ag_m_sdl_Window_sdl_fillRect(AgSdlWindow* thiz, AgSdlRect* r, int64_t color) {
    SDL_SetRenderDrawColor(thiz->sdl_rend, (int)color, (int)color >> 8, (int)color >> 16, (int)color >> 24);
    SDL_RenderFillRect(thiz->sdl_rend, &(SDL_Rect){ (int)r->x, (int)r->y, (int)r->w, (int)r->h });
}

void ag_m_sdl_Window_sdl_bltXYWH(AgSdlWindow* thiz, AgSdlTexture* tex, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh) {
    SDL_RenderCopy(
        thiz->sdl_rend,
        tex->sdl_tex,
        &(SDL_Rect){ (int)sx, (int)sy, (int)sw, (int)sh },
        &(SDL_Rect){ (int)dx, (int)dy, (int)dw, (int)dh });
}

void ag_m_sdl_Window_sdl_blt(AgSdlWindow* thiz, AgSdlTexture* tex, AgSdlRect* s, AgSdlRect* d) {
    SDL_RenderCopy(
        thiz->sdl_rend,
        tex->sdl_tex,
        &(SDL_Rect){ (int)s->x, (int)s->y, (int)s->w, (int)s->h },
        & (SDL_Rect) {
        (int)d->x, (int)d->y, (int)d->w, (int)d->h
    });
}

void ag_m_sdl_Window_sdl_flip(AgSdlWindow* thiz) {
    SDL_RenderPresent(thiz->sdl_rend);
}

bool ag_m_sdl_Texture_sdl_load(AgSdlTexture* tex, AgSdlWindow* w, AgString* file_name) {
    return ag_sdl_surface_to_texture(tex, w, IMG_Load(file_name->ptr));
}

bool ag_sdl_surface_to_texture(AgSdlTexture* tex, AgSdlWindow* w, SDL_Surface* s) {
    ag_destroy_sdl_Texture(tex);
    if (s) {
        tex->sdl_tex = SDL_CreateTextureFromSurface(w->sdl_rend, s);
        tex->w = s->w;
        tex->h = s->h;
        SDL_FreeSurface(s);
    } else {
        tex->sdl_tex = NULL;
        tex->w = tex->h = 0;
    }
    return tex->sdl_tex != NULL;
}

void ag_destroy_sdl_Texture(AgSdlTexture* tex) {
    if (tex->sdl_tex)
        SDL_DestroyTexture(tex->sdl_tex);
}

void ag_m_sdl_Texture_sdl_setAlphaMod(AgSdlTexture* tex, int64_t multiplier) {
    if (tex->sdl_tex)
        SDL_SetTextureAlphaMod(tex->sdl_tex, (int)multiplier);
}

void ag_m_sdl_Texture_sdl_setTextureColorMod(AgSdlTexture* tex, int64_t color) {
    if (tex->sdl_tex)
        SDL_SetTextureColorMod(
            tex->sdl_tex,
            (int8_t)(color & 0xff),
            (int8_t)(color >> 8) & 0xff,
            (int8_t)(color >> 16) & 0xff);
}

bool ag_m_sdl_Font_sdl_load(AgSdlFont* thiz, AgString* fontName, int style) {
    thiz->sdl_font = TTF_OpenFontIndex(fontName->ptr, 16, style);
    return thiz->sdl_font != NULL;
}

AgString* ag_m_sdl_Font_sdl_name(AgSdlFont* thiz) {
    const char* r = thiz->sdl_font
        ? TTF_FontFaceFamilyName(thiz->sdl_font)
        : "";
    return ag_make_str(r, strlen(r));
}

AgString* ag_m_sdl_Font_sdl_style(AgSdlFont* thiz) {
    const char* r = thiz->sdl_font
        ? TTF_FontFaceStyleName(thiz->sdl_font)
        : "";
    return ag_make_str(r, strlen(r));
}

int64_t ag_m_sdl_Font_sdl_stylesCount(AgSdlFont* thiz) {
    return  thiz->sdl_font ? TTF_FontFaces(thiz->sdl_font) : 0;
}

void ag_ttf_set_style(AgSdlFont* f, int ptSize, int flags) {
    if (f->last_pt_size != ptSize) {
        TTF_SetFontSize(f->sdl_font, ptSize);
        f->last_pt_size = ptSize;
    }
    if (f->last_flags != flags) {
        TTF_SetFontStyle(f->sdl_font, flags);
        f->last_flags = flags;
    }
}

void ag_m_sdl_Font_sdl_renderTo(AgSdlFont* thiz, AgSdlTexture* r, AgString* str, int ptSize, int flags, AgSdlWindow* wnd) {
    if (!thiz->sdl_font)
        return;
    ag_ttf_set_style(thiz, ptSize, flags);
    ag_sdl_surface_to_texture(
        r,
        wnd,
        TTF_RenderUTF8_Shaded(
            thiz->sdl_font,
            str->ptr,
            (SDL_Color) { 255, 255, 255, 255 },
            (SDL_Color) { 0, 0, 0, 0 }));

}

int64_t ag_m_sdl_Font_sdl_fit(AgSdlFont* thiz, AgString* s, int64_t ptSize, int64_t flags, int64_t width) {
    if (!thiz->sdl_font)
        return 0;
    ag_ttf_set_style(thiz, ptSize, flags);
    int unused, count = 0;
    TTF_MeasureUTF8(thiz->sdl_font, s->ptr, width, &unused, &count);
    return count;
}

int64_t ag_m_sdl_Font_sdl_measure(AgSdlFont* thiz, AgString* str, int64_t ptSize, int64_t flags, AgSdlRect* extents) {
    if (!thiz->sdl_font)
        return 0;
    ag_ttf_set_style(thiz, ptSize, flags);
    extents->y = -TTF_FontAscent(thiz->sdl_font);
    extents->h = TTF_FontDescent(thiz->sdl_font) - extents->y;
    const char* s = str->ptr;
    int first_char = get_utf8(&s);
    int minx, unused_maxx, unused_miny, unused_maxy, unused_adv;
    TTF_GlyphMetrics32(thiz->sdl_font, first_char, &minx, &unused_maxx, &unused_miny, &unused_maxy, &unused_adv);
    extents->x = minx;
    int width = 0, unused_height;
    TTF_SizeText(thiz->sdl_font, str->ptr, &width, &unused_height);
    extents->w = width;
    int extent = 0, unused_count;
    TTF_MeasureUTF8(thiz->sdl_font, str->ptr, INT_MAX, &extent, &unused_count);
    return extent;
}
