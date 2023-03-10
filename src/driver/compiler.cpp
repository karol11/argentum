#include <iostream>
#include <fstream>
#include <optional>

#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "compiler/ast.h"
#include "compiler/parser.h"
#include "compiler/name-resolver.h"
#include "compiler/type-checker.h"
#include "compiler/generator.h"
#include "utils/runtime.h"

using ltm::own;
using ast::Ast;
using std::optional;

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
    llvm::InitLLVM X(argc, argv);
    try {
        if (argc < 4) {
            if (argc > 1 && std::string(argv[1]) == "--message") {
                std::cout << "Argentum compiler by Andrey Kamlatskiy." << std::endl;
                return 0;
            } else {
                std::cout
                    << "Usage: " << argv[0] << " path_to_sources start_module_name output_obj_file <flags>" << std::endl
                    << "--help for more info." << std::endl;
            }
            return 0;
        }
        ast::initialize();
        auto ast = own<Ast>::make();
        runtime::register_content(*ast);
        std::unordered_set<ltm::pin<dom::Name>> modules_in_dep_path;
        parse(ast, ast->dom->names()->get(argv[2]), modules_in_dep_path, [&](auto name) {
            return read_file(std::string(argv[1]) + "/" + std::to_string(name) + ".ag");
        });
        resolve_names(ast);
        check_types(ast);
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        auto threadsafe_module = generate_code(ast);
        threadsafe_module.withModuleDo([&](llvm::Module& module) {
            auto target_triple = llvm::sys::getDefaultTargetTriple();
            module.setTargetTriple(target_triple);
            std::string error_str;
            auto target = llvm::TargetRegistry::lookupTarget(target_triple, error_str);
            if (!target) {
                llvm::errs() << error_str << "\n";
                exit(1);
            }
            auto target_machine = target->createTargetMachine(
                target_triple,
                "generic",  // cpu
                "",         // features
                llvm::TargetOptions(),
                llvm::Optional<llvm::Reloc::Model>());
            module.setDataLayout(target_machine->createDataLayout());
            std::error_code err_code;
            llvm::raw_fd_ostream out_file(argv[3], err_code, llvm::sys::fs::OF_None);
            if (err_code) {
                llvm::errs() << "Could not open file: " << err_code.message() << "\n";
                exit(1);
            }
            llvm::legacy::PassManager pass_manager;
            if (target_machine->addPassesToEmitFile(pass_manager, out_file, nullptr, llvm::CGFT_ObjectFile)) {
                llvm::errs() << "llvm can't emit a file of this type for target " << target_triple << "\n";
                exit(1);
            }
            pass_manager.run(module);
            out_file.flush();
            llvm::outs() << "Done " << argv[3] << "\n";
        });
//    } catch (void*) {  // debug-only  TODO: replace exceptions with `quick_exit`
    } catch (int) {
        return -1;
    }
    return 0;
}
