#ifndef _AK_CONST_CAPTURE_PASS_H_
#define _AK_CONST_CAPTURE_PASS_H_

#include "compiler/ast.h"

void const_capture_pass(ltm::pin<ast::Ast> ast);

#endif  // _AK_CONST_CAPTURE_PASS_H_
