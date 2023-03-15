#include <iostream>
#include <fstream>

#include "SDL.h"
#include "SDL_image.h"
#include "compiler/ast.h"
#include "dom/dom-to-string.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/type-checker.h"
#include "utils/register_runtime.h"
#include "runtime/runtime.h"

using ltm::own;
using ast::Ast;
int64_t generate_and_execute(ltm::pin<Ast> ast, bool dump_ir);  // defined in `generator.h/cpp`

void to_log(AgString* str) {
    std::cout << str->ptr << std::endl;
}
int64_t create_window(AgString* title, int64_t x, int64_t y, int64_t w, int64_t h, int64_t flags) {
    return (int64_t) SDL_CreateWindow(title->ptr, int(x), int(y), int(w), int(h), int(flags));
}
void make_fit(AgBlob* b, size_t required_size) {
    if (b->size * sizeof(int64_t) < required_size)
        ag_fn_sys_Container_insert(b, b->size, required_size - b->size * sizeof(int64_t));
}
void wait_event(AgBlob* event) {
    make_fit(event, sizeof(SDL_Event));
    SDL_WaitEvent((SDL_Event*)event->data);
}
void poll_event(AgBlob* event) {
    make_fit(event, sizeof(SDL_Event));
    SDL_PollEvent((SDL_Event*)event->data);
}
void fill_rect(SDL_Renderer* renderer, int64_t x, int64_t y, int64_t w, int64_t h) {
    SDL_Rect r{ int(x), int(y), int(w), int(h) };
    SDL_RenderFillRect(renderer, &r);
}
int64_t img_load(AgString* file_name) {
    return (int64_t) IMG_Load(file_name->ptr);
}
void img_blt(SDL_Renderer* renderer, SDL_Texture* texture, int64_t sx, int64_t sy, int64_t sw, int64_t sh, int64_t dx, int64_t dy, int64_t dw, int64_t dh) {
    SDL_Rect s{ int(sx), int(sy), int(sw), int(sh) };
    SDL_Rect d{ int(dx), int(dy), int(dw), int(dh) };
    SDL_RenderCopy(renderer, texture, &s, &d);
}

void set_color_mod(SDL_Texture* texture, int64_t color) {
    SDL_SetTextureColorMod(texture, int8_t(color & 0xff), int8_t(color >> 8) & 0xff, int8_t(color >> 16) & 0xff);
}

std::string read_file(std::string file_name) {
    std::ifstream f(file_name, std::ios::binary | std::ios::ate);
    if (f) {
        std::string r(f.tellg(), '\0');
        f.seekg(0, std::ios::beg);
        f.read(r.data(), r.size());
        return r;
    } else {
        std::cerr << "Can't read :" << file_name << std::endl;
        throw 1;
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cout << "Usage: " << argv[0] << " path_to_sources start_module_name" << std::endl;
            return 0;
        }
        if (std::string(argv[1]) == "--help") {
            std::cout << "Argentum JIT interpreter demo. Language that makes difference." << std::endl;
            return 0;
        }
        ast::initialize();
        auto ast = own<Ast>::make();
        using FN = void(*)();
        ast->platform_exports.insert({
            { "ag_fn_sys_log", FN(to_log) },
            { "ag_fn_sys_sdl_init", FN(SDL_Init) },
            { "ag_fn_sys_sdl_createWindow", FN(create_window) },
            { "ag_fn_sys_sdl_createRenderer", FN(SDL_CreateRenderer) },
            { "ag_fn_sys_sdl_waitEvent", FN(wait_event) },
            { "ag_fn_sys_sdl_pollEvent", FN(poll_event) },
            { "ag_fn_sys_sdl_setRendererDrawColor", FN(SDL_SetRenderDrawColor) },
            { "ag_fn_sys_sdl_rendererClear", FN(SDL_RenderClear) },
            { "ag_fn_sys_sdl_rendererFillRect", FN(fill_rect) },
            { "ag_fn_sys_sdl_rendererPresent", FN(SDL_RenderPresent) },
            { "ag_fn_sys_sdl_createTextureFromSurface", FN(SDL_CreateTextureFromSurface) },
            { "ag_fn_sys_sdl_setTextureAlphaMod", FN(SDL_SetTextureAlphaMod) },
            { "ag_fn_sys_sdl_setTextureColorMod", FN(set_color_mod)},
            { "ag_fn_sys_sdl_blt", FN(img_blt) },
            { "ag_fn_sys_sdl_freeSurface", FN(SDL_FreeSurface) },
            { "ag_fn_sys_sdl_destroyTexture", FN(SDL_DestroyTexture)},
            { "ag_fn_sys_sdl_destroyRenderer", FN(SDL_DestroyRenderer) },
            { "ag_fn_sys_sdl_destroyWindow", FN(SDL_DestroyWindow) },
            { "ag_fn_sys_sdl_delay", FN(SDL_Delay) },
            { "ag_fn_sys_sdl_quit", FN(SDL_Quit) },
            { "ag_fn_sys_img_init", FN(IMG_Init) },
            { "ag_fn_sys_img_load", FN(img_load) },
            { "ag_fn_sys_img_quit", FN(IMG_Quit) }});
        std::cout << "Parsing " << argv[1] << std::endl;
        std::unordered_set<ltm::pin<dom::Name>> modules_in_dep_path;
        parse(ast, ast->dom->names()->get(argv[2]), modules_in_dep_path, [&](auto name) {
            return read_file(std::string(argv[1]) + "/" + std::to_string(name) + ".ag");
        });
        std::cout << "Checking name consistency" << std::endl;
        resolve_names(ast);
        std::cout << "Checking types" << std::endl;
        check_types(ast);
        std::cout << "Building bitcode" << std::endl;
        return int(generate_and_execute(ast, false));
    } catch (void*) {  // debug-only  TODO: replace exceptions with `quick_exit`
//    } catch (int) {
        return -1;
    }
    return 0;
}
