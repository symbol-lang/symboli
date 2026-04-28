#ifndef AST_H
#define AST_H

#include "types.h"

/* Binary operator codes (values above 255 to avoid ASCII clash) */
#define BINARY_EQ      256
#define BINARY_NEQ     257
#define BINARY_LE      258
#define BINARY_GE      259
#define BINARY_AND     260
#define BINARY_OR      261
#define BINARY_SHL     262
#define BINARY_SHR     263
/* single-char bitwise use ASCII: '&'=38, '|'=124, '^'=94 */

/* Unary operator codes */
#define UNARY_PRE_INC  300
#define UNARY_PRE_DEC  301
#define UNARY_POST_INC 302
#define UNARY_POST_DEC 303
#define UNARY_BIT_NOT  304

typedef enum ASTKind {
	AST_VAR_DECL       = 0,
	AST_LAMBDA         = 1,
	AST_CALL           = 2,
	AST_LITERAL        = 3,
	AST_VAR_REF        = 4,
	AST_STRING_INTERP  = 5,
	AST_PROGRAM        = 6,
	AST_INTERFACE_DECL = 7,
	AST_OBJECT_LITERAL = 8,
	AST_MEMBER_ACCESS  = 9,
	AST_RETURN         = 10,
	AST_BINARY         = 11,
	AST_IMPORT_DECL    = 12,
	AST_EXPORT_DECL    = 13,
	AST_IF             = 14,
	AST_WHILE          = 15,
	AST_DO_WHILE       = 16,
	AST_FOR            = 17,
	AST_SWITCH         = 18,
	AST_BREAK          = 19,
	AST_ASSIGN         = 20,
	AST_CONTINUE       = 26,
	AST_UNARY          = 21,
	AST_CONDITIONAL    = 22,
	AST_ARRAY_LITERAL  = 23,
	AST_INDEX_ACCESS   = 24,
	AST_INDEX_ASSIGN   = 25,
	AST_ENUM_DECL      = 27,
	AST_MEMBER_ASSIGN  = 28,
} ASTKind;

typedef struct AST {
	ASTKind kind;
	int line;
	int col;
	union {
		struct {
			char* name;
			struct Type* vartype;
			struct AST* init;
			int is_const;
		} var_decl;
		struct {
			struct Type* ret_type;
			char** param_names;
			struct Type** param_types;
			struct AST** param_defaults;
			int param_count;
			struct AST** body;
			int body_count;
		} lambda;
		struct {
			struct AST* cond;
			struct AST* then_branch;
			struct AST* else_branch;
		} if_stmt;
		struct {
			struct AST* cond;
			struct AST* body;
		} while_stmt;
		struct {
			struct AST* cond;
			struct AST* true_branch;
			struct AST* false_branch;
		} conditional;
		struct {
			struct AST* body;
			struct AST* cond;
		} do_while_stmt;
		struct {
			struct AST* init;
			struct AST* cond;
			struct AST* update;
			struct AST* body;
		} for_stmt;
		struct {
			struct AST* expr;
			struct AST** case_values;
			struct AST** case_bodies;
			int case_count;
			struct AST* default_body;
		} switch_stmt;
		// struct {
		// } break_stmt;
		struct {
			struct AST* func;
			struct AST** args;
			int arg_count;
		} call;
		struct {
			struct Type* type;
			union {
				int b;
				int i;
				double f;
				char* s;
			} val;
		} literal;
		char* var_ref;
		struct {
			char* str;          /* template: ${0}, ${1}, ... markers */
			struct AST** exprs; /* parsed expression AST nodes       */
			int expr_count;
		} string_interp;
		struct {
			struct AST** body;
			int body_count;
			int is_block; /* 1 = explicit { } block, 0 = top-level program */
		} program;
		struct {
			char* name;
			struct Type* iface_type;
		} interface_decl;
		struct {
			char** names;
			struct AST** values;
			int field_count;
		} object_literal;
		struct {
			struct AST* object;
			char* member;
		} member_access;
		struct {
			struct AST* expr;
		} return_stmt;
		struct {
			int op;
			struct AST* left;
			struct AST* right;
		} binary;
		struct {
			char* name;
			struct AST* value;
		} assign;
		struct {
			int op;
			struct AST* operand;
		} unary;
		struct {
			char** names;
			int name_count;
			char* source;
		} import_decl;
		struct {
			char** names;
			int name_count;
		} export_decl;
		struct {
			struct AST** values;
			int count;
			struct Type* array_type;
		} array_literal;
		struct {
			struct AST* object;
			struct AST* index;
		} index_access;
		struct {
			struct AST* object;
			struct AST* index;
			struct AST* value;
		} index_assign;
		struct {
			char* name;
			char** member_names;
			struct AST** member_values; /* NULL entry = auto-increment */
			int member_count;
		} enum_decl;
		struct {
			struct AST* object;
			char* member;
			struct AST* value;
		} member_assign;
	} u;
} AST;

#endif
