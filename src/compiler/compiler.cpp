#include <iostream>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "ast.h"
#include "parser.h"
#include "name-resolver.h"
#include "type-checker.h"
#include "pruner.h"
#include "const-capture-pass.h"
#include "generator.h"
#include "utils/register_runtime.h"

using ltm::own;
using ast::Ast;
using std::optional;
using std::nullopt;
using std::string;
using std::vector;

optional<string> read_file(string file_name) {
    std::ifstream f(file_name, std::ios::binary | std::ios::ate);
    if (f) {
        std::string r(f.tellg(), '\0');
        f.seekg(0, std::ios::beg);
        f.read(r.data(), r.size());
        return r;
    } else {
        return nullopt;
    }
}

vector<string> src_dir_names;
string read_source(string moduleName, string& path) {
    string last_dir;
    auto handle_dir = [&](string dir) -> optional<string> {
        path = ast::format_str(dir, "/", moduleName, ".ag");
        if (auto r = read_file(path))
            return r;
        string ref_path = ast::format_str(dir, "/", moduleName, ".ag-ref");
        if (auto ref = read_file(ref_path)) {
            path = std::filesystem::path(ast::format_str(dir, "/", *ref)).lexically_normal().generic_string();
            if (auto r = read_file(path))
                return r;
            std::cerr << "Can't read :" << path << " by ref " << ref_path << std::endl;
            panic();
        }
        return nullopt;
    };
    for (const auto& dir: src_dir_names) {
        last_dir = dir;
        if (auto r = handle_dir(dir))
            return *r;
    }
    while (auto parent = read_file(ast::format_str(last_dir, "/sdk.ref"))) {
        path = std::filesystem::path(ast::format_str(last_dir, "/", *parent)).lexically_normal().generic_string();
        src_dir_names.push_back(path);
        if (auto r = handle_dir(path))
            return *r;
        last_dir = *parent;
    }
    std::cerr << "Can't read : " << moduleName << std::endl;
    panic();
}

int main(int argc, char* argv[]) {
    llvm::InitLLVM X(argc, argv);
    if (argc < 2) {
        llvm::outs() <<
                "Argentum compiler by Andrey Kamlatskiy.\n"
                "Usage: " << argv[0] << " -src path_to_sources -start module_name -o output_file <flags>\n"
                "--help for more info.\n";
        return 0;
    }
    auto target_triple = llvm::sys::getDefaultTargetTriple();
    bool output_bitcode = false;
    bool output_asm = false;
    bool add_debug_info = false;
    bool test_mode = false;
    string start_module_name, out_file_name, opt_level;
    string entry_point_name = "main";
    for (auto arg = argv + 1, end = argv + argc; arg != end; arg++) {
        auto param = [&] {
            if (++arg == end) {
                llvm::errs() << "expected flag parameter\n";
                exit(1);
            }
            return *arg;
        };
        if (strcmp(*arg, "--help") == 0) {
            llvm::outs() <<
                "Flags\n"
                "  -src directory     : where sources of all modules are located.\n"
                "  -start module_name : what module is a start module.\n"
                "  -o out_file        : file to store object file or asm or bitcode.\n"
                "  -target <arch><sub>-<vendor>-<sys>-<abi>\n"
                "          Example: x86_64-unknown-linux-gnu\n"
                "                or x86_64-w64-microsoft-windows\n"
                "  -g         : generate debug info\n"
                "  -emit-llvm : output bitcode\n"
                "  -ON        : optimize 0-none, 1-less, 2-default, 3-aggressive\n"
                "  -e fn_name : entry point fn name (default `main`)\n"
                "  -T         : build tests\n"
                "  -S         : output asm file\n";
            return 0;
        } else if (strcmp(*arg, "-S") == 0) {
            output_asm = true;
        } else if (strcmp(*arg, "-emit-llvm") == 0) {
            output_bitcode = true;
        } else if (strcmp(*arg, "-g") == 0) {
            add_debug_info = true;
        } else if (strncmp(*arg, "-O", 2) == 0) {
            opt_level = (*arg) + 2;
        } else if (strcmp(*arg, "-target") == 0) {
            target_triple = param();
        } else if (strcmp(*arg, "-e") == 0) {
            entry_point_name = param();
        } else if (strcmp(*arg, "-T") == 0) {
            test_mode = true;
        } else if (strcmp(*arg, "-o") == 0) {
            out_file_name = param();
        } else if (strcmp(*arg, "-start") == 0) {
            start_module_name = param();
        } else if (strcmp(*arg, "-src") == 0) {
            src_dir_names.push_back(param());
        } else {
            llvm::errs() << "unexpected cmdline argument " << *arg << "\n";
            exit(1);
        }
    }
    auto check_str = [](string& s, const char* name) {
        if (s.empty()) {
            llvm::errs() << name << " name is not provided\n";
            exit(1);
        }
    };
    if (src_dir_names.empty()) {
        llvm::errs() << "at least one -src parameter expected\n";
        exit(1);
    }
    check_str(start_module_name, "start module");
    check_str(out_file_name, "output file");
    ast::initialize();
    auto ast = own<Ast>::make();
    ast->test_mode = test_mode;
    register_runtime_content(*ast);
    parse(ast, start_module_name, [&](auto name, auto& out_path) {
        return read_source(name, out_path);
    });
    resolve_names(ast);
    check_types(ast);
    prune(ast);
    const_capture_pass(ast);
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    auto threadsafe_module = generate_code(ast, add_debug_info, entry_point_name);
    threadsafe_module.withModuleDo([&](llvm::Module& module) {
        std::error_code err_code;
        llvm::raw_fd_ostream out_file(out_file_name, err_code, llvm::sys::fs::OF_None);
        if (err_code) {
            llvm::errs() << "Could not write file: " << err_code.message() << "\n";
            exit(1);
        }
        module.setTargetTriple(target_triple);
        if (output_bitcode) {
            if (output_asm)
                module.print(out_file, nullptr);
            else
                llvm::WriteBitcodeToFile(module, out_file);
        } else {
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
                std::optional<llvm::Reloc::Model>());
            if (add_debug_info && opt_level.empty())
                opt_level = "0";
            if (!opt_level.empty())
                target_machine->setOptLevel(
                    opt_level == "0" ? llvm::CodeGenOpt::Level::None :
                    opt_level == "1" ? llvm::CodeGenOpt::Level::Less :
                    opt_level == "3" ? llvm::CodeGenOpt::Level::Aggressive :
                    llvm::CodeGenOpt::Level::Default);
            module.setDataLayout(target_machine->createDataLayout());
            llvm::legacy::PassManager pass_manager;
            if (target_machine->addPassesToEmitFile(pass_manager, out_file, nullptr, output_asm
                ? llvm::CGFT_AssemblyFile
                : llvm::CGFT_ObjectFile)) {
                llvm::errs() << "llvm can't emit a file of this type for target " << target_triple << "\n";
                exit(1);
            }
            pass_manager.run(module);
        }
        out_file.flush();
        llvm::outs() << "Done " << out_file_name << "\n";
    });
    return 0;
}

[[noreturn]] void panic() {
    std::quick_exit(1);
}