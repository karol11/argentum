#ifndef _AK_PARSER_H_
#define _AK_PARSER_H_

#include "compiler/ast.h"

void parse(
	ltm::pin<ast::Ast> ast,
	ltm::pin<dom::Name> module_name,
	std::unordered_set<ltm::pin<dom::Name>>& modules_in_dep_path,
	const std::function<std::string (ltm::pin<dom::Name> name)>& module_text_provider);

#endif  // _AK_PARSER_H_
