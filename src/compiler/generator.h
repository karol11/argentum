#ifndef _AK_GENERATOR_H_
#define _AK_GENERATOR_H_

#include "compiler/ast.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

llvm::orc::ThreadSafeModule generate_code(ltm::pin<ast::Ast> ast, bool add_debug_info);

int64_t execute(llvm::orc::ThreadSafeModule module, bool dump_ir = false);

int64_t generate_and_execute(ltm::pin<ast::Ast> ast, bool add_debug_info, bool dump_ir);  // used without import in `compiler-test.cpp`

#endif  // _AK_GENERATOR_H_
