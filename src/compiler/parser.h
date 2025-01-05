#ifndef _AK_PARSER_H_
#define _AK_PARSER_H_

#include "ast.h"

void parse(
	ltm::pin<ast::Ast> ast,
	std::string start_module_name,
	const std::function<std::string (std::string name, std::string& out_dir)>& module_text_provider);

#endif  // _AK_PARSER_H_
