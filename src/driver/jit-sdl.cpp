#include <iostream>
#include <fstream>

#include "compiler/ast.h"
#include "dom/dom-to-string.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/const-capture-pass.h"
#include "compiler/pruner.h"
#include "compiler/type-checker.h"
#include "utils/register_runtime.h"

#include "runtime/runtime.h"
#include "runtime/sdl-bindings.h"

using ltm::own;
using ast::Ast;
int64_t generate_and_execute(ltm::pin<Ast> ast, bool add_debug_info, bool dump_ir);  // defined in `generator.h/cpp`

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

#define AG_STR(N) #N
#define FN(N) { AG_STR(N), (void(*)()) N } 

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
            FN(ag_m_sdl_Sdl_sdl_init),
            FN(ag_m_sdl_Sdl_sdl_pollEvent),
            FN(ag_fn_sdl_disposeSdl),
            FN(ag_m_sdl_Window_sdl_init),
            FN(ag_fn_sdl_disposeWindow),
            FN(ag_m_sdl_Window_sdl_fill),
            FN(ag_m_sdl_Window_sdl_fillRect),
            FN(ag_m_sdl_Window_sdl_blt),
            FN(ag_m_sdl_Window_sdl_flip),
            FN(ag_m_sdl_Texture_sdl_load),
            FN(ag_fn_sdl_disposeTexture),
            FN(ag_m_sdl_Texture_sdl_setAlphaMod),
            FN(ag_m_sdl_Texture_sdl_setColorMod),
            FN(ag_m_sdl_Font_sdl_load),
            FN(ag_fn_sdl_disposeFont),
            FN(ag_m_sdl_Font_sdl_name),
            FN(ag_m_sdl_Font_sdl_style),
            FN(ag_m_sdl_Font_sdl_stylesCount),
            FN(ag_m_sdl_Font_sdl_renderTo),
            FN(ag_m_sdl_Font_sdl_fit),
            FN(ag_m_sdl_Font_sdl_measure) });
        std::cout << "Parsing " << argv[1] << std::endl;
        parse(ast, argv[2], [&](auto name) {
            return read_file(ast::format_str(argv[1], "/", name, ".ag"));
        });
        std::cout << "Checking name consistency" << std::endl;
        resolve_names(ast);
        std::cout << "Checking types" << std::endl;
        check_types(ast);
        std::cout << "Building bitcode" << std::endl;
        const_capture_pass(ast);
        prune(ast);
        generate_and_execute(ast, false, false);  // no debug info, no dump
//    } catch (void*) {  // debug-only  TODO: replace exceptions with `quick_exit`
    } catch (int) {
        return -1;
    }
    return 0;
}
