#include <iostream>
#include <fstream>

#include "compiler/ast.h"
#include "dom/dom-to-string.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
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
            FN(ag_fn_sdlFfi_sdlInit),
            FN(ag_fn_sdlFfi_createWindow),
            FN(ag_fn_sdlFfi_createRenderer),
            FN(ag_fn_sdlFfi_waitEvent),
            FN(ag_fn_sdlFfi_pollEvent),
            FN(ag_fn_sdlFfi_setRendererDrawColor),
            FN(ag_fn_sdlFfi_rendererClear),
            FN(ag_fn_sdlFfi_rendererFillRect),
            FN(ag_fn_sdlFfi_rendererPresent),
            FN(ag_fn_sdlFfi_createTextureFromSurface),
            FN(ag_fn_sdlFfi_setTextureAlphaMod),
            FN(ag_fn_sdlFfi_setTextureColorMod),
            FN(ag_fn_sdlFfi_blt),
            FN(ag_fn_sdlFfi_freeSurface),
            FN(ag_fn_sdlFfi_destroyTexture),
            FN(ag_fn_sdlFfi_destroyRenderer),
            FN(ag_fn_sdlFfi_destroyWindow),
            FN(ag_fn_sdlFfi_delay),
            FN(ag_fn_sdlFfi_sdlQuit),
            FN(ag_fn_sdlFfi_imgInit),
            FN(ag_fn_sdlFfi_imgLoad),
            FN(ag_fn_sdlFfi_imgQuit) });
        std::cout << "Parsing " << argv[1] << std::endl;
        parse(ast, argv[2], [&](auto name) {
            return read_file(ast::format_str(argv[1], "/", name, ".ag"));
        });
        std::cout << "Checking name consistency" << std::endl;
        resolve_names(ast);
        std::cout << "Checking types" << std::endl;
        check_types(ast);
        std::cout << "Building bitcode" << std::endl;
        generate_and_execute(ast, false, false);  // no debug info, no dump
//    } catch (void*) {  // debug-only  TODO: replace exceptions with `quick_exit`
    } catch (int) {
        return -1;
    }
    return 0;
}
