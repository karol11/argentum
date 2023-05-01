#ifndef _AK_NAME_RESOLVER_H_
#define _AK_NAME_RESOLVER_H_

#include "compiler/ast.h"

void resolve_names(ltm::pin<ast::Ast> ast);

void resolve_immediate_delegate(ltm::pin<ast::Ast> ast, ast::ImmediateDelegate& node, ltm::pin<ast::Class> cls);

#endif  // _AK_NAME_RESOLVER_H_
