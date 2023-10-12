#include "../runtime/runtime.h"
#include "../runtime/blob.h"
#include "../runtime/sdl-bindings.h"

#include "SDL.h"
#include "SDL_image.h"

int64_t ag_fn_sdlFfi_sdlInit(int64_t flags) {
    return SDL_Init((int)flags);
}

void ag_fn_sdlFfi_sdlQuit() {
    SDL_Quit();
}

int64_t ag_fn_sdlFfi_createWindow(AgString* title, int64_t x, int64_t y, int64_t w, int64_t h, int64_t flags) {
    return (int64_t) SDL_CreateWindow(title->ptr, (int)x, (int)y, (int)w, (int)h, (int)flags);
}

void ag_fn_sdlFfi_destroyWindow(int64_t windowId) {
    SDL_DestroyWindow((SDL_Window*)windowId);
}

int64_t ag_fn_sdlFfi_createRenderer(int64_t windowId, int64_t index, int64_t flags) {
    return (int64_t) SDL_CreateRenderer((SDL_Window*)windowId, (int)index, (int)flags);
}

void ag_fn_sdlFfi_destroyRenderer(int64_t rendererId) {
    SDL_DestroyRenderer((SDL_Renderer*)rendererId);
}

void ag_fn_sdlFfi_waitEvent(AgBlob* event) {
    ag_make_blob_fit(event, sizeof(SDL_Event));
    SDL_WaitEvent((SDL_Event*)event->data);
}

void ag_fn_sdlFfi_pollEvent(AgBlob* event) {
    ag_make_blob_fit(event, sizeof(SDL_Event));
    SDL_PollEvent((SDL_Event*)event->data);
}

void ag_fn_sdlFfi_delay(int64_t millisec) {
    SDL_Delay((int)millisec);
}

void ag_fn_sdlFfi_setRendererDrawColor(int64_t rendererId, int64_t r, int64_t g, int64_t b, int64_t a) {
    SDL_SetRenderDrawColor((SDL_Renderer*)rendererId, (int)r, (int)g, (int)b, (int)a);
}

void ag_fn_sdlFfi_rendererClear(int64_t rendererId) {
    SDL_RenderClear((SDL_Renderer*)rendererId);
}

void ag_fn_sdlFfi_rendererFillRect(int64_t rendererId, int64_t x, int64_t y, int64_t w, int64_t h) {
    SDL_RenderFillRect(
        (SDL_Renderer*)rendererId,
        &(SDL_Rect){ (int)x, (int)y, (int)w, (int)h });
}

void ag_fn_sdlFfi_blt(int64_t rendererId, int64_t textureId, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh) {
    SDL_RenderCopy(
        (SDL_Renderer*) rendererId,
        (SDL_Texture*) textureId,
        &(SDL_Rect){ (int)sx, (int)sy, (int)sw, (int)sh },
        &(SDL_Rect){ (int)dx, (int)dy, (int)dw, (int)dh });
}

void ag_fn_sdlFfi_rendererPresent(int64_t rendererId) {
    SDL_RenderPresent((SDL_Renderer*)rendererId);
}

int64_t ag_fn_sdlFfi_createTextureFromSurface(int64_t rendererId, int64_t surfaceId) {
    return (int64_t) SDL_CreateTextureFromSurface((SDL_Renderer*)rendererId, (SDL_Surface*)surfaceId);
}

void ag_fn_sdlFfi_destroyTexture(int64_t textureId) {
    SDL_DestroyTexture((SDL_Texture*) textureId);
}

void ag_fn_sdlFfi_setTextureAlphaMod(int64_t textureId, int64_t multiplier) {
    SDL_SetTextureAlphaMod((SDL_Texture*)textureId, (int)multiplier);
}

void ag_fn_sdlFfi_setTextureColorMod(int64_t textureId, int64_t color) {
    SDL_SetTextureColorMod(
        (SDL_Texture*) textureId,
        (int8_t)(color & 0xff),
        (int8_t)(color >> 8) & 0xff,
        (int8_t)(color >> 16) & 0xff);
}

void ag_fn_sdlFfi_freeSurface(int64_t surfaceId) {
    SDL_FreeSurface((SDL_Surface*)surfaceId);
}

int64_t ag_fn_sdlFfi_imgInit(int64_t flags) {
    return IMG_Init((int)flags);
}

void ag_fn_sdlFfi_imgQuit() {
    IMG_Quit();
}

int64_t ag_fn_sdlFfi_imgLoad(AgString* file_name) {
    return (int64_t)IMG_Load(file_name->ptr);
}
