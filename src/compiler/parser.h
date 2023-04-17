#ifndef _AK_PARSER_H_
#define _AK_PARSER_H_

#include "compiler/ast.h"

ltm::pin<ast::Module> parse(
	ltm::pin<ast::Ast> ast,
	std::string module_name,
	std::unordered_set<std::string>& modules_in_dep_path,
	const std::function<std::string (std::string name)>& module_text_provider);

#endif  // _AK_PARSER_H_
