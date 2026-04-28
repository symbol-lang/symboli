#ifndef EVAL_H
#define EVAL_H

#include "types.h"
#include "ast.h"

Value* eval(AST* ast, Env** env);

void eval_set_filename(const char* filename);

#endif
