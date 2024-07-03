#include <limits.h>
#include "sdl-bindings.h"
#include "utf8.h"

#include "SDL.h"
#include "SDL_image.h"
#include "SDL_ttf.h"

void** ag_disp_sdl_Font(uint64_t interface_and_method_ordinal);
void** ag_disp_sdl_Texture(uint64_t interface_and_method_ordinal);

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
    AgWeak* wnd;
    TTF_Font* sdl_font;
    int64_t last_pt_size;
    int64_t last_flags;
} AgSdlFont;

bool ag_sdl_surface_to_texture(AgSdlTexture* tex, AgSdlWindow* w, SDL_Surface* s) {
    ag_fn_sdl_disposeTexture(tex);
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

bool ag_m_sdl_Sdl_sdl_init(AgSdl* thiz, int64_t sdl_flags, int64_t img_flags) {
    ag_make_blob_fit(thiz->event, sizeof(SDL_Event));
    bool r = 
        SDL_Init((int)sdl_flags) == 0 &&
        IMG_Init((int)img_flags) == img_flags &&
        TTF_Init() == 0;
    if(!r)
        fprintf(stdout, "Error: %s\n", SDL_GetError());
    return r;
}

AgBlob* ag_m_sdl_Sdl_sdl_pollEvent(AgSdl* thiz) {
    return SDL_PollEvent((SDL_Event*)thiz->event->bytes)
        ? ag_retain_pin(&thiz->event->head), thiz->event
        : NULL;
}

void ag_fn_sdl_disposeSdl(AgSdl* thiz) {
    if (thiz->event->bytes_count) {
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
    }
}

bool ag_m_sdl_Window_sdl_init(AgSdlWindow* thiz, AgString* title, AgSdlRect* bounds, int64_t flags, int64_t r_flags) {
    thiz->sdl_wnd = SDL_CreateWindow(title->chars, (int)bounds->x, (int)bounds->y, (int)bounds->w, (int)bounds->h, (int)flags);
    thiz->sdl_rend = SDL_CreateRenderer(thiz->sdl_wnd, 0, (int)r_flags);
    return thiz->sdl_wnd && thiz->sdl_rend;
}

void ag_fn_sdl_disposeWindow(AgSdlWindow* thiz) {
    if (thiz->sdl_rend)
        SDL_DestroyRenderer(thiz->sdl_rend);
    if (thiz->sdl_wnd)
        SDL_DestroyWindow(thiz->sdl_wnd);
}

AgSdlFont* ag_m_sdl_Window_sdl_loadFont(AgSdlWindow *thiz, AgString* fileName, int style) {
    AgSdlFont* r = (AgSdlFont*) ag_allocate_obj(sizeof(AgSdlFont));
    r->base.dispatcher = ag_disp_sdl_Font;
    r->base.ctr_mt |= AG_CTR_SHARED;
    r->sdl_font = TTF_OpenFontIndex(fileName->chars, 16, style);
    if (!r->sdl_font) {
        ag_free(r);
        return NULL;
    }
    r->wnd = ag_mk_weak(&thiz->base);
    return r;
}

AgSdlTexture* ag_m_sdl_Window_sdl_loadTexture(AgSdlWindow* thiz, AgString* fileName) {
    AgSdlTexture* r = (AgSdlTexture*) ag_allocate_obj(sizeof(AgSdlTexture));
    r->base.dispatcher = ag_disp_sdl_Texture;
    r->base.ctr_mt |= AG_CTR_SHARED;
    if (ag_sdl_surface_to_texture(r, thiz, IMG_Load(fileName->chars)))
        return r;
    ag_release_shared(&r->base);
    return NULL;
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

void ag_sdl_set_tex_color(SDL_Texture* sdl_tex, int64_t color) {
    SDL_SetTextureAlphaMod(sdl_tex, (int8_t) (color >> 24));
    SDL_SetTextureColorMod(
        sdl_tex,
        (int8_t)((color >> 16) & 0xff),
        (int8_t)((color >> 8) & 0xff),
        (int8_t)color & 0xff);        
}

void ag_m_sdl_Window_sdl_bltXYWH(
        AgSdlWindow* thiz, AgSdlTexture* tex,
        int64_t sx, int64_t sy, int64_t sw, int64_t sh,
        int64_t dx, int64_t dy, int64_t dw, int64_t dh,
        int64_t color) {
    ag_sdl_set_tex_color(tex->sdl_tex, color);
    SDL_RenderCopy(
        thiz->sdl_rend,
        tex->sdl_tex,
        &(SDL_Rect){ (int)sx, (int)sy, (int)sw, (int)sh },
        &(SDL_Rect){ (int)dx, (int)dy, (int)dw, (int)dh });
}

void ag_m_sdl_Window_sdl_blt(AgSdlWindow* thiz, AgSdlTexture* tex, AgSdlRect* s, AgSdlRect* d, int64_t color) {
    ag_sdl_set_tex_color(tex->sdl_tex, color);
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

void ag_fn_sdl_disposeTexture(AgSdlTexture* tex) {
    if (tex->sdl_tex)
        SDL_DestroyTexture(tex->sdl_tex);
}

void ag_fn_sdl_disposeFont(AgSdlFont* fnt) {
    if (fnt->sdl_font)
        TTF_CloseFont(fnt->sdl_font);
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

// returns *Texture
AgSdlTexture* ag_m_sdl_Font_sdl_render(AgSdlFont* thiz, AgString* str, int ptSize, int flags) {
    AgSdlTexture* r = (AgSdlTexture*) ag_allocate_obj(sizeof(AgSdlTexture));
    r->base.dispatcher = ag_disp_sdl_Texture;
    r->base.ctr_mt |= AG_CTR_SHARED;
    if (thiz->sdl_font) {
        AgSdlWindow* w = (AgSdlWindow*) ag_deref_weak(thiz->wnd);
        if (w) {
            ag_ttf_set_style(thiz, ptSize, flags);
            ag_sdl_surface_to_texture(
                r,
                w,
                TTF_RenderUTF8_Blended(
                    thiz->sdl_font,
                    str->chars,
                    (SDL_Color) { 255, 255, 255, 255 }));
            ag_release_pin_nn(&w->base);
        }
    }
    return r;
}

int64_t ag_m_sdl_Font_sdl_fit(AgSdlFont* thiz, AgString* s, int64_t ptSize, int64_t flags, int64_t width) {
    if (!thiz->sdl_font)
        return 0;
    ag_ttf_set_style(thiz, (int) ptSize, (int) flags);
    int unused, count = 0;
    TTF_MeasureUTF8(thiz->sdl_font, s->chars, (int) width, &unused, &count);
    return count;
}

int64_t ag_m_sdl_Font_sdl_measure(AgSdlFont* thiz, AgString* str, int64_t ptSize, int64_t flags, AgSdlRect* extents) {
    if (!thiz->sdl_font)
        return 0;
    ag_ttf_set_style(thiz, (int) ptSize, (int) flags);
    extents->y = -TTF_FontAscent(thiz->sdl_font);
    extents->h = TTF_FontDescent(thiz->sdl_font) - extents->y;
    const char* s = str->chars;
    int first_char = get_utf8(&s);
    int minx, unused_maxx, unused_miny, unused_maxy, unused_adv;
    TTF_GlyphMetrics32(thiz->sdl_font, first_char, &minx, &unused_maxx, &unused_miny, &unused_maxy, &unused_adv);
    extents->x = minx;
    int width = 0, unused_height;
    TTF_SizeText(thiz->sdl_font, str->chars, &width, &unused_height);
    extents->w = width;
    int extent = 0, unused_count;
    TTF_MeasureUTF8(thiz->sdl_font, str->chars, INT_MAX, &extent, &unused_count);
    return extent;
}
