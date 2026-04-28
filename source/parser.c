#include "parser.h"

#include "types.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ParsedSignature {
	int is_function;
	Type* ret_type;
	char** param_names;
	Type** param_types;
	AST** param_defaults;
	int param_count;
} ParsedSignature;

typedef struct NamedType {
	char* name;
	Type* type;
} NamedType;

static const char* src;
static int pos;
static int length;
static int cur_line;
static int cur_col;
static NamedType* named_types;
static int named_type_count;
static int parse_failed;
static int error_count;
static int block_depth;

#define BINARY_AND	   260
#define BINARY_OR	   261
#define UNARY_PRE_INC  300
#define UNARY_PRE_DEC  301
#define UNARY_POST_INC 302
#define UNARY_POST_DEC 303

static int starts_with(const char* s);
static int starts_with_keyword(const char* s);
static AST* parse_statement(void);
static AST* parse_block(void);
static AST* parse_if(void);
static AST* parse_while(void);
static AST* parse_do_while(void);
static AST* parse_for(void);
static AST* parse_switch(void);
static AST* parse_break(void);
static AST* parse_expression(void);
static AST* parse_assign(void);
static AST* parse_conditional(void);
static AST* parse_logical_or(void);
static AST* parse_logical_and(void);
static AST* parse_bitwise_or(void);
static AST* parse_bitwise_xor(void);
static AST* parse_bitwise_and(void);
static AST* parse_equality(void);
static AST* parse_comparison(void);
static AST* parse_shift(void);
static AST* parse_addition(void);
static AST* parse_term(void);
static AST* parse_unary(void);
static AST* parse_postfix(void);
static AST* parse_primary(void);
static void skip_ws(void);
static int match(const char* s);
static char* parse_identifier(void);
static void parse_error(const char* fmt, ...);
static void sync_to_next_statement(void);
static Type* parse_type(void);
static int is_valid_statement(AST* stmt);
static Type* parse_type(void);

static char** parse_name_list(int* out_count) {
	char** names = NULL;
	*out_count = 0;

	if (!match("{")) return NULL;
	skip_ws();
	if (match("}")) return names;

	while (pos < length) {
		char* name = parse_identifier();
		if (!name) break;
		names = realloc(names, sizeof(char*) * (size_t)(*out_count + 1));
		names[*out_count] = name;
		(*out_count)++;
		skip_ws();
		if (match(",")) continue;
		break;
	}

	match("}");
	return names;
}

static void register_named_type(char* name, Type* type) {
	named_types = realloc(named_types,
						  sizeof(NamedType) * (size_t)(named_type_count + 1));
	named_types[named_type_count].name = strdup(name);
	named_types[named_type_count].type = type;
	named_type_count++;
}

static Type* lookup_named_type(const char* name) {
	for (int i = named_type_count - 1; i >= 0; i--) {
		if (strcmp(named_types[i].name, name) == 0) return named_types[i].type;
	}
	return NULL;
}

static void skip_ws(void) {
	while (pos < length) {
		if (isspace((unsigned char)src[pos])) {
			if (src[pos] == '\n') {
				cur_line++;
				cur_col = 1;
			} else {
				cur_col++;
			}
			pos++;
			continue;
		}
		if (starts_with("//")) {
			while (pos < length && src[pos] != '\n')
				pos++;
			continue;
		}
		if (starts_with("/*")) {
			int level = 1;
			pos += 2;
			while (pos < length && level > 0) {
				if (starts_with("/*")) {
					level++;
					pos += 2;
				} else if (starts_with("*/")) {
					level--;
					pos += 2;
				} else {
					if (src[pos] == '\n') {
						cur_line++;
						cur_col = 1;
					} else {
						cur_col++;
					}
					pos++;
				}
			}
			continue;
		}
		break;
	}
}

static const char* ast_kind_name(ASTKind k) {
	switch (k) {
		case AST_VAR_DECL:
			return "variable declaration";
		case AST_LAMBDA:
			return "function expression";
		case AST_CALL:
			return "function call";
		case AST_LITERAL:
			return "literal value";
		case AST_VAR_REF:
			return "variable reference";
		case AST_STRING_INTERP:
			return "string interpolation";
		case AST_PROGRAM:
			return "program";
		case AST_INTERFACE_DECL:
			return "interface declaration";
		case AST_OBJECT_LITERAL:
			return "object literal";
		case AST_MEMBER_ACCESS:
			return "member access";
		case AST_MEMBER_ASSIGN:
			return "member assignment";
		case AST_RETURN:
			return "return statement";
		case AST_BINARY:
			return "binary expression";
		case AST_IMPORT_DECL:
			return "import declaration";
		case AST_EXPORT_DECL:
			return "export declaration";
		case AST_IF:
			return "if statement";
		case AST_WHILE:
			return "while loop";
		case AST_DO_WHILE:
			return "do-while loop";
		case AST_FOR:
			return "for loop";
		case AST_SWITCH:
			return "switch statement";
		case AST_BREAK:
			return "break statement";
		case AST_CONTINUE:
			return "continue statement";
		case AST_ASSIGN:
			return "assignment";
		case AST_UNARY:
			return "unary expression";
		case AST_CONDITIONAL:
			return "conditional expression";
		case AST_ARRAY_LITERAL:
			return "array literal";
		case AST_INDEX_ACCESS:
			return "index access";
		case AST_INDEX_ASSIGN:
			return "index assignment";
		case AST_ENUM_DECL:
			return "enum declaration";
	}
	return "expression";
}

/* Extract up to `maxlen` chars of the source line at `line_num` (1-based). */
static void source_line_snippet(int line_num, int col, char* out, int maxlen) {
	if (line_num < 1) line_num = 1;
	int cur = 1, start = 0;
	for (int i = 0; i < length; i++) {
		if (cur == line_num) {
			start = i;
			break;
		}
		if (src[i] == '\n') cur++;
	}
	/* trim leading whitespace */
	while (start < length && (src[start] == ' ' || src[start] == '\t'))
		start++;

	int end = start;
	while (end < length && src[end] != '\n')
		end++;

	int avail = maxlen - 4; /* room for "..." + NUL */
	int snippet_len = end - start;
	if (snippet_len > avail) {
		memcpy(out, src + start, (size_t)avail);
		memcpy(out + avail, "...", 4);
	} else {
		memcpy(out, src + start, (size_t)snippet_len);
		out[snippet_len] = '\0';
	}
	(void)col;
}

static void parse_error(const char* fmt, ...) {
	parse_failed = 1;
	error_count++;

	char message[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	fprintf(stderr,
			"Syntax error at line %d, column %d: %s\n",
			cur_line,
			cur_col,
			message);
}

/* Used inside blocks: stops at '}' so we don't consume the block closer. */
static void sync_to_next_statement(void) {
	parse_failed = 0;
	while (pos < length) {
		if (src[pos] == '}') return;
		if (src[pos] == '\n') {
			pos++;
			cur_line++;
			cur_col = 1;
			return;
		}
		pos++;
		cur_col++;
	}
}

/* Used at the top level: always advances past the current line (including '}').
   Without this, a stray '}' at column 0 would loop forever. */
static void sync_to_next_line(void) {
	parse_failed = 0;
	while (pos < length && src[pos] != '\n') {
		pos++;
		cur_col++;
	}
	if (pos < length) {
		pos++;
		cur_line++;
		cur_col = 1;
	}
}

static int is_valid_statement(AST* stmt) {
	if (!stmt) return 0;
	switch (stmt->kind) {
		case AST_VAR_DECL:
		case AST_LAMBDA:
		case AST_CALL:
		case AST_INTERFACE_DECL:
		case AST_ENUM_DECL:
		case AST_RETURN:
		case AST_IMPORT_DECL:
		case AST_EXPORT_DECL:
		case AST_IF:
		case AST_WHILE:
		case AST_DO_WHILE:
		case AST_FOR:
		case AST_SWITCH:
		case AST_BREAK:
		case AST_CONTINUE:
		case AST_ASSIGN:
		case AST_UNARY:
		case AST_INDEX_ASSIGN:
		case AST_MEMBER_ASSIGN:
			return 1;
		default:
			return 0;
	}
}

static int starts_with(const char* s) {
	size_t len = strlen(s);
	if (pos + (int)len > length) return 0;
	return strncmp(src + pos, s, len) == 0;
}

static int is_identifier_char(char c) {
	return isalnum((unsigned char)c) || c == '_';
}

static int starts_with_keyword(const char* s) {
	size_t len = strlen(s);
	if (!starts_with(s)) return 0;
	if (pos + (int)len >= length) return 1;
	return !is_identifier_char(src[pos + len]);
}

static int match(const char* s) {
	skip_ws();
	if (starts_with(s)) {
		int len = (int)strlen(s);
		for (int i = 0; i < len; i++) {
			if (src[pos] == '\n') {
				cur_line++;
				cur_col = 0;
			} else {
				cur_col++;
			}
			pos++;
		}
		return 1;
	}
	return 0;
}

static AST* make_ast(ASTKind kind) {
	AST* ast = malloc(sizeof(AST));
	ast->kind = kind;
	ast->line = cur_line;
	ast->col = cur_col;
	return ast;
}

static char* parse_identifier(void) {
	skip_ws();
	int start = pos;
	if (pos < length && (isalpha((unsigned char)src[pos]) || src[pos] == '_')) {
		pos++;
		while (pos < length && is_identifier_char(src[pos]))
			pos++;
		int len = pos - start;
		for (int i = 0; i < len; i++) {
			cur_col++;
		}
		char* id = malloc((size_t)len + 1);
		memcpy(id, src + start, (size_t)len);
		id[len] = '\0';
		return id;
	}
	return NULL;
}

static Type* parse_union_type(void);

static int is_known_type_name(const char* name) {
	if (strcmp(name, "null") == 0 || strcmp(name, "bool") == 0
		|| strcmp(name, "int") == 0 || strcmp(name, "float") == 0
		|| strcmp(name, "string") == 0 || strcmp(name, "any") == 0)
		return 1;
	return lookup_named_type(name) != NULL;
}

static Type* parse_named_or_basic_type(void) {
	char* name = parse_identifier();
	Type* type = NULL;

	if (!name) return NULL;

	if (strcmp(name, "null") == 0)
		type = make_basic(BASIC_NULL);
	else if (strcmp(name, "bool") == 0)
		type = make_basic(BASIC_BOOL);
	else if (strcmp(name, "int") == 0)
		type = make_basic(BASIC_INT);
	else if (strcmp(name, "float") == 0)
		type = make_basic(BASIC_FLOAT);
	else if (strcmp(name, "string") == 0)
		type = make_basic(BASIC_STRING);
	else if (strcmp(name, "any") == 0)
		type = make_basic(BASIC_ANY);
	else {
		type = lookup_named_type(name);
		if (!type) parse_error("Unknown type '%s'", name);
	}

	free(name);
	return type;
}

static Type** parse_type_arg_list(int* out_count) {
	Type** params = NULL;
	*out_count = 0;

	if (!match("(")) return NULL;
	skip_ws();
	if (match(")")) return NULL;

	while (pos < length) {
		Type* param = NULL;
		int save = pos, save_line = cur_line, save_col = cur_col;
		char* maybe_name = parse_identifier();
		if (maybe_name) {
			skip_ws();
			if (match(":")) {
				param = parse_type();
			} else {
				pos = save;
				cur_line = save_line;
				cur_col = save_col;
				param = parse_type();
			}
			free(maybe_name);
		}

		if (!param) break;
		params = realloc(params, sizeof(Type*) * (size_t)(*out_count + 1));
		params[*out_count] = param;
		(*out_count)++;

		skip_ws();
		if (match(",")) continue;
		break;
	}

	match(")");
	return params;
}

static Type* parse_union_type(void) {
	Type* first = parse_named_or_basic_type();
	if (!first) return NULL;

	while (1) {
		skip_ws();
		if (!match("[")) break;
		if (!match("]")) return first;
		first = make_array(first);
	}

	skip_ws();
	if (!starts_with("|")) return first;

	Type** types = malloc(sizeof(Type*));
	types[0] = first;
	int count = 1;

	while (starts_with("|")) {
		pos++;
		skip_ws();
		Type* next = parse_named_or_basic_type();
		if (!next) break;
		while (1) {
			skip_ws();
			if (!match("[")) break;
			if (!match("]")) break;
			next = make_array(next);
		}
		types = realloc(types, sizeof(Type*) * (size_t)(count + 1));
		types[count++] = next;
		skip_ws();
	}

	if (count == 1) {
		free(types);
		return first;
	}
	return make_union(types, count);
}

static Type* parse_inline_interface_type(void) {
	if (!match("{")) return NULL;
	skip_ws();
	InterfaceField* fields = NULL;
	int field_count = 0;
	while (pos < length && !starts_with("}")) {
		char* field_name = parse_identifier();
		if (!field_name) break;
		skip_ws();
		match(":");
		skip_ws();
		Type* field_type = parse_type();
		fields =
			realloc(fields, sizeof(InterfaceField) * (size_t)(field_count + 1));
		fields[field_count].name = field_name;
		fields[field_count].type = field_type;
		field_count++;
		skip_ws();
		match(",");
		skip_ws();
	}
	match("}");
	return make_interface(NULL, fields, field_count);
}

static Type* parse_type(void) {
	skip_ws();
	if (starts_with("{")) return parse_inline_interface_type();
	Type* type = parse_union_type();

	skip_ws();
	if (starts_with("(")) {
		if (!type)
			type =
				make_basic(BASIC_NULL); /* () shorthand: default return null */
		int param_count = 0;
		Type** params = parse_type_arg_list(&param_count);
		type = make_func(type, params, param_count);
	}

	return type;
}

/* Double-quoted string: supports \n, multiline, ${} interpolation */
static char* parse_string_literal(void) {
	skip_ws();
	if (pos >= length || src[pos] != '"') return NULL;
	pos++;
	int start = pos;
	while (pos < length && src[pos] != '"') {
		if (src[pos] == '\n') {
			cur_line++;
			cur_col = 0;
			pos++;
		} else if (src[pos] == '\\' && pos + 1 < length)
			pos += 2;
		else {
			cur_col++;
			pos++;
		}
	}
	int raw_len = pos - start;
	char* buffer = malloc((size_t)raw_len + 1);
	int ri = 0;
	for (int i = start; i < pos; i++) {
		if (src[i] == '\\' && i + 1 < pos) {
			i++;
			switch (src[i]) {
				case 'n':
					buffer[ri++] = '\n';
					break;
				case 't':
					buffer[ri++] = '\t';
					break;
				case 'r':
					buffer[ri++] = '\r';
					break;
				case '\\':
					buffer[ri++] = '\\';
					break;
				case '"':
					buffer[ri++] = '"';
					break;
				case '$':
					buffer[ri++] = '$';
					break;
				default:
					buffer[ri++] = '\\';
					buffer[ri++] = src[i];
					break;
			}
		} else {
			buffer[ri++] = src[i];
		}
	}
	buffer[ri] = '\0';
	if (pos < length && src[pos] == '"') pos++;
	return buffer;
}

/* Single-quoted string: plain text, no interpolation, no newlines */
static char* parse_single_string_literal(void) {
	skip_ws();
	if (pos >= length || src[pos] != '\'') return NULL;
	pos++;
	int start = pos;
	while (pos < length && src[pos] != '\'' && src[pos] != '\n') {
		if (src[pos] == '\\' && pos + 1 < length && src[pos + 1] != '\n')
			pos += 2;
		else
			pos++;
	}
	int raw_len = pos - start;
	char* buffer = malloc((size_t)raw_len + 1);
	int ri = 0;
	for (int i = start; i < pos; i++) {
		if (src[i] == '\\' && i + 1 < pos) {
			i++;
			switch (src[i]) {
				case 'n':
					buffer[ri++] = 'n';
					break; /* \n is literal 'n' */
				case 't':
					buffer[ri++] = '\t';
					break;
				case 'r':
					buffer[ri++] = '\r';
					break;
				case '\\':
					buffer[ri++] = '\\';
					break;
				case '\'':
					buffer[ri++] = '\'';
					break;
				default:
					buffer[ri++] = '\\';
					buffer[ri++] = src[i];
					break;
			}
		} else {
			buffer[ri++] = src[i];
		}
	}
	buffer[ri] = '\0';
	if (pos < length && src[pos] == '\'') pos++;
	return buffer;
}

static AST* make_literal_ast(Type* type) {
	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_LITERAL;
	ast->u.literal.type = type;
	return ast;
}

static AST* parse_interpolated_string(void) {
	skip_ws();
	if (pos >= length || src[pos] != '"') return NULL;
	pos++; // skip opening quote

	char* template = NULL;
	int template_len = 0;
	int template_cap = 0;

	AST** exprs = NULL;
	int expr_count = 0;
	int expr_cap = 0;

	int expr_index = 0;

	while (pos < length && src[pos] != '"') {
		if (src[pos] == '\\' && pos + 1 < length) {
			// Handle escape sequences
			pos++;
			char c = src[pos++];
			switch (c) {
				case 'n':
					c = '\n';
					break;
				case 't':
					c = '\t';
					break;
				case 'r':
					c = '\r';
					break;
				case '\\':
					c = '\\';
					break;
				case '"':
					c = '"';
					break;
				case '$':
					c = '$';
					break;
				default:
					break;
			}
			// Add to template
			if (template_len + 1 >= template_cap) {
				template_cap = template_cap == 0 ? 16 : template_cap * 2;
				template = realloc(template, (size_t)template_cap);
			}
			template[template_len++] = c;
		} else if (src[pos] == '$' && pos + 1 < length && src[pos + 1] == '{') {
			// Found interpolation
			pos += 2; // skip ${

			// Add placeholder to template
			char placeholder[32];
			int plen =
				snprintf(placeholder, sizeof(placeholder), "${%d}", expr_index);
			if (template_len + plen >= template_cap) {
				template_cap = template_cap == 0 ? 16 : template_cap * 2;
				while (template_len + plen >= template_cap)
					template_cap *= 2;
				template = realloc(template, (size_t)template_cap);
			}
			memcpy(template + template_len, placeholder, (size_t)plen);
			template_len += plen;

			// Parse the expression
			AST* expr = parse_expression();
			if (!expr) {
				// Error: invalid expression in interpolation
				free(template);
				for (int i = 0; i < expr_count; i++)
					free(exprs[i]);
				free(exprs);
				return NULL;
			}

			// Add expression to list
			if (expr_count >= expr_cap) {
				expr_cap = expr_cap == 0 ? 4 : expr_cap * 2;
				exprs = realloc(exprs, (size_t)expr_cap * sizeof(AST*));
			}
			exprs[expr_count++] = expr;
			expr_index++;

			// Expect closing }
			skip_ws();
			if (pos >= length || src[pos] != '}') {
				// Error: missing closing }
				free(template);
				for (int i = 0; i < expr_count; i++)
					free(exprs[i]);
				free(exprs);
				return NULL;
			}
			pos++; // skip }
		} else {
			// Regular character
			if (src[pos] == '\n') {
				cur_line++;
				cur_col = 0;
			} else
				cur_col++;

			if (template_len + 1 >= template_cap) {
				template_cap = template_cap == 0 ? 16 : template_cap * 2;
				template = realloc(template, (size_t)template_cap);
			}
			template[template_len++] = src[pos++];
		}
	}

	if (pos < length && src[pos] == '"') pos++; // skip closing quote

	// Null terminate template
	if (template_len + 1 >= template_cap) {
		template_cap = template_cap == 0 ? 1 : template_cap * 2;
		template = realloc(template, (size_t)template_cap);
	}
	template[template_len] = '\0';

	// If no interpolations, return a simple string literal
	if (expr_count == 0) {
		free(exprs);
		AST* ast = make_literal_ast(make_basic(BASIC_STRING));
		ast->u.literal.val.s = template;
		return ast;
	}

	// Create interpolated string AST
	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_STRING_INTERP;
	ast->u.string_interp.str = template;
	ast->u.string_interp.exprs = exprs;
	ast->u.string_interp.expr_count = expr_count;
	return ast;
}

static AST* parse_block(void) {
	if (!match("{")) return NULL;

	AST** body = NULL;
	int body_count = 0;

	block_depth++;
	while (1) {
		skip_ws();
		if (pos >= length || starts_with("}")) break;
		AST* stmt = parse_statement();
		if (!stmt) {
			if (!parse_failed)
				parse_error("Unexpected token '%c' in block",
							pos < length ? src[pos] : '?');
			sync_to_next_statement();
			if (pos >= length || starts_with("}")) break;
			continue;
		}
		body = realloc(body, sizeof(AST*) * (size_t)(body_count + 1));
		body[body_count++] = stmt;
	}
	block_depth--;

	match("}");

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_PROGRAM;
	ast->u.program.body = body;
	ast->u.program.body_count = body_count;
	ast->u.program.is_block = 1;
	return ast;
}

static AST* parse_if(void) {
	if (!match("if")) return NULL;
	match("(");
	AST* cond = parse_expression();
	if (!cond) {
		parse_error("Expected expression in if condition");
		return NULL;
	}
	if (!match(")")) {
		parse_error("Expected ')' after if condition");
		return NULL;
	}
	AST* then_branch = parse_statement();
	if (!then_branch) {
		if (!parse_failed) parse_error("Expected statement after if condition");
		return NULL;
	}
	AST* else_branch = NULL;
	skip_ws();
	if (starts_with_keyword("else")) {
		pos += 4;
		else_branch = parse_statement();
		if (!else_branch && !parse_failed) {
			parse_error("Expected statement after else");
			return NULL;
		}
	}

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_IF;
	ast->u.if_stmt.cond = cond;
	ast->u.if_stmt.then_branch = then_branch;
	ast->u.if_stmt.else_branch = else_branch;
	return ast;
}

static AST* parse_while(void) {
	if (!match("while")) return NULL;
	match("(");
	AST* cond = parse_expression();
	if (!cond) {
		parse_error("Expected expression in while condition");
		return NULL;
	}
	if (!match(")")) {
		parse_error("Expected ')' after while condition");
		return NULL;
	}
	AST* body = parse_statement();
	if (!body) {
		if (!parse_failed)
			parse_error("Expected statement after while condition");
		return NULL;
	}

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_WHILE;
	ast->u.while_stmt.cond = cond;
	ast->u.while_stmt.body = body;
	return ast;
}

static AST* parse_do_while(void) {
	if (!match("do")) return NULL;
	AST* body = parse_statement();
	match("while");
	match("(");
	AST* cond = parse_expression();
	match(")");

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_DO_WHILE;
	ast->u.do_while_stmt.body = body;
	ast->u.do_while_stmt.cond = cond;
	return ast;
}

static AST* parse_for(void) {
	if (!match("for")) return NULL;
	match("(");

	AST* init = NULL;
	AST* cond = NULL;
	AST* update = NULL;

	if (!match(";")) {
		init = parse_statement();
		match(";");
	}

	if (!match(";")) {
		cond = parse_expression();
		match(";");
	}

	if (!match(")")) {
		update = parse_expression();
		match(")");
	}

	AST* body = parse_statement();

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_FOR;
	ast->u.for_stmt.init = init;
	ast->u.for_stmt.cond = cond;
	ast->u.for_stmt.update = update;
	ast->u.for_stmt.body = body;
	return ast;
}

static AST* parse_switch(void) {
	if (!match("switch")) return NULL;
	match("(");
	AST* expr = parse_expression();
	match(")");
	if (!match("{")) return NULL;

	AST** case_values = NULL;
	AST** case_bodies = NULL;
	int case_count = 0;
	AST* default_body = NULL;

	while (pos < length && !starts_with("}")) {
		skip_ws();
		if (starts_with_keyword("case")) {
			pos += 4;
			AST* value = parse_expression();
			match(":");
			AST* body = NULL;
			if (starts_with("{"))
				body = parse_block();
			else
				body = parse_statement();

			case_values =
				realloc(case_values, sizeof(AST*) * (size_t)(case_count + 1));
			case_bodies =
				realloc(case_bodies, sizeof(AST*) * (size_t)(case_count + 1));
			case_values[case_count] = value;
			case_bodies[case_count] = body;
			case_count++;
			continue;
		}

		if (starts_with_keyword("default")) {
			pos += 7;
			match(":");
			if (starts_with("{"))
				default_body = parse_block();
			else
				default_body = parse_statement();
			continue;
		}

		break;
	}

	match("}");

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_SWITCH;
	ast->u.switch_stmt.expr = expr;
	ast->u.switch_stmt.case_values = case_values;
	ast->u.switch_stmt.case_bodies = case_bodies;
	ast->u.switch_stmt.case_count = case_count;
	ast->u.switch_stmt.default_body = default_body;
	return ast;
}

static AST* parse_break(void) {
	if (!match("break")) return NULL;
	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_BREAK;
	return ast;
}

static AST* parse_continue(void) {
	if (!match("continue")) return NULL;
	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_CONTINUE;
	return ast;
}

static AST* parse_term(void) {
	AST* left = parse_unary();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		int op = 0;
		if (starts_with("*")) {
			pos++;
			op = '*';
		} else if (starts_with("/")) {
			pos++;
			op = '/';
		} else if (starts_with("%")) {
			pos++;
			op = '%';
		} else {
			break;
		}

		AST* right = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = op;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_addition(void) {
	AST* left = parse_term();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		int op = 0;
		if (match("+")) {
			op = '+';
		} else if (match("-")) {
			op = '-';
		} else {
			break;
		}
		AST* right = parse_term();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = op;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_shift(void) {
	AST* left = parse_addition();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		int op = 0;
		if (starts_with("<<") && !starts_with("<<=")) {
			pos += 2;
			op = BINARY_SHL;
		} else if (starts_with(">>") && !starts_with(">>=")) {
			pos += 2;
			op = BINARY_SHR;
		} else {
			break;
		}
		AST* right = parse_addition();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = op;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_comparison(void) {
	AST* left = parse_shift();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		int op = 0;
		if (starts_with("<=")) {
			pos += 2;
			op = 258;
		} else if (starts_with(">=")) {
			pos += 2;
			op = 259;
		} else if (starts_with("<") && !starts_with("<=")) {
			pos++;
			op = '<';
		} else if (starts_with(">") && !starts_with(">=")) {
			pos++;
			op = '>';
		} else {
			break;
		}

		AST* right = parse_addition();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = op;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_equality(void) {
	AST* left = parse_comparison();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		int op = 0;
		if (starts_with("==")) {
			pos += 2;
			op = 256;
		} else if (starts_with("!=")) {
			pos += 2;
			op = 257;
		} else {
			break;
		}

		AST* right = parse_comparison();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = op;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_bitwise_and(void) {
	AST* left = parse_equality();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		/* '&' but not '&&' or '&=' */
		if (!starts_with("&") || starts_with("&&") || starts_with("&=")) break;
		pos++;
		AST* right = parse_equality();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = '&';
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_bitwise_xor(void) {
	AST* left = parse_bitwise_and();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		if (!starts_with("^") || starts_with("^=")) break;
		pos++;
		AST* right = parse_bitwise_and();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = '^';
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_bitwise_or(void) {
	AST* left = parse_bitwise_xor();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		/* '|' but not '||' or '|=' */
		if (!starts_with("|") || starts_with("||") || starts_with("|=")) break;
		pos++;
		AST* right = parse_bitwise_xor();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = '|';
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_logical_and(void) {
	AST* left = parse_bitwise_or();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		if (!starts_with("&&")) break;
		pos += 2;
		AST* right = parse_bitwise_or();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = BINARY_AND;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_logical_or(void) {
	AST* left = parse_logical_and();
	if (!left) return NULL;

	while (1) {
		skip_ws();
		if (!starts_with("||")) break;
		pos += 2;
		AST* right = parse_logical_and();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_BINARY;
		ast->u.binary.op = BINARY_OR;
		ast->u.binary.left = left;
		ast->u.binary.right = right;
		left = ast;
	}

	return left;
}

static AST* parse_conditional(void) {
	AST* cond = parse_logical_or();
	if (!cond) return NULL;

	skip_ws();
	if (!match("?")) return cond;

	AST* true_branch = parse_expression();
	if (!match(":")) {
		parse_error("Expected ':' in conditional expression");
		return NULL;
	}
	AST* false_branch = parse_conditional();

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_CONDITIONAL;
	ast->u.conditional.cond = cond;
	ast->u.conditional.true_branch = true_branch;
	ast->u.conditional.false_branch = false_branch;
	return ast;
}

static AST* parse_assign(void) {
	int save = pos, save_line = cur_line, save_col = cur_col;
	skip_ws();
	char* name = parse_identifier();
	if (name) {
		skip_ws();

		/* Compound assignment: +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>= */
		int compound_op = 0;
		int compound_len = 2;
		if (starts_with("<<=")) {
			compound_op = BINARY_SHL;
			compound_len = 3;
		} else if (starts_with(">>=")) {
			compound_op = BINARY_SHR;
			compound_len = 3;
		} else if (starts_with("+=")) {
			compound_op = '+';
		} else if (starts_with("-=")) {
			compound_op = '-';
		} else if (starts_with("*=")) {
			compound_op = '*';
		} else if (starts_with("/=")) {
			compound_op = '/';
		} else if (starts_with("%=")) {
			compound_op = '%';
		} else if (starts_with("&=")) {
			compound_op = '&';
		} else if (starts_with("|=")) {
			compound_op = '|';
		} else if (starts_with("^=")) {
			compound_op = '^';
		}

		if (compound_op) {
			pos += compound_len;
			AST* value = parse_assign();
			AST* left = malloc(sizeof(AST));
			left->kind = AST_VAR_REF;
			left->u.var_ref = strdup(name);
			AST* bin = malloc(sizeof(AST));
			bin->kind = AST_BINARY;
			bin->u.binary.op = compound_op;
			bin->u.binary.left = left;
			bin->u.binary.right = value;
			AST* ast = malloc(sizeof(AST));
			ast->kind = AST_ASSIGN;
			ast->u.assign.name = name;
			ast->u.assign.value = bin;
			return ast;
		}

		if (starts_with("=") && !starts_with("==")) {
			pos++;
			AST* value = parse_assign();
			AST* ast = malloc(sizeof(AST));
			ast->kind = AST_ASSIGN;
			ast->u.assign.name = name;
			ast->u.assign.value = value;
			return ast;
		}

		/* name[idx] = value  — index assignment */
		if (starts_with("[")) {
			int save2 = pos, save2_line = cur_line, save2_col = cur_col;
			match("[");
			AST* idx = parse_expression();
			if (idx && match("]")) {
				skip_ws();
				if (starts_with("=") && !starts_with("==")) {
					pos++;
					AST* value = parse_assign();
					AST* obj_ref = make_ast(AST_VAR_REF);
					obj_ref->u.var_ref = strdup(name);
					free(name);
					AST* ast = make_ast(AST_INDEX_ASSIGN);
					ast->u.index_assign.object = obj_ref;
					ast->u.index_assign.index = idx;
					ast->u.index_assign.value = value;
					return ast;
				}
			}
			pos = save2;
			cur_line = save2_line;
			cur_col = save2_col; /* backtrack */
		}

		free(name);
	}
	pos = save;
	cur_line = save_line;
	cur_col = save_col;
	AST* lhs = parse_conditional();
	skip_ws();
	if (lhs && lhs->kind == AST_MEMBER_ACCESS
		&& starts_with("=") && !starts_with("==")) {
		pos++;
		AST* value = parse_assign();
		AST* ast = make_ast(AST_MEMBER_ASSIGN);
		ast->u.member_assign.object = lhs->u.member_access.object;
		ast->u.member_assign.member = lhs->u.member_access.member;
		ast->u.member_assign.value = value;
		lhs->u.member_access.object = NULL;
		lhs->u.member_access.member = NULL;
		free(lhs);
		return ast;
	}
	return lhs;
}

static AST* parse_number_literal(void) {
	skip_ws();
	int start = pos;
	int saw_dot = 0;

	while (pos < length && isdigit((unsigned char)src[pos]))
		pos++;
	if (pos < length && src[pos] == '.') {
		saw_dot = 1;
		pos++;
		while (pos < length && isdigit((unsigned char)src[pos]))
			pos++;
	}

	if (start == pos) return NULL;

	int len = pos - start;
	char buffer[64];
	if (len >= (int)sizeof(buffer)) len = (int)sizeof(buffer) - 1;
	memcpy(buffer, src + start, (size_t)len);
	buffer[len] = '\0';

	if (saw_dot) {
		AST* ast = make_literal_ast(make_basic(BASIC_FLOAT));
		ast->u.literal.val.f = strtod(buffer, NULL);
		return ast;
	}

	AST* ast = make_literal_ast(make_basic(BASIC_INT));
	ast->u.literal.val.i = atoi(buffer);
	return ast;
}

static void parse_param_list(char*** out_names, Type*** out_types,
							 AST*** out_defaults, int* out_count) {
	*out_names = NULL;
	*out_types = NULL;
	*out_defaults = NULL;
	*out_count = 0;

	if (!match("(")) return;
	skip_ws();
	if (match(")")) return;

	while (pos < length) {
		char* name = parse_identifier();
		Type* type = NULL;
		AST* def = NULL;
		if (!name) break;
		skip_ws();
		if (match(":")) type = parse_type();
		if (!type)
			type =
				make_basic(BASIC_ANY); /* unannotated param defaults to any */
		skip_ws();
		if (starts_with("=") && !starts_with("==")) {
			pos++;
			def = parse_comparison();
		}

		*out_names =
			realloc(*out_names, sizeof(char*) * (size_t)(*out_count + 1));
		*out_types =
			realloc(*out_types, sizeof(Type*) * (size_t)(*out_count + 1));
		*out_defaults =
			realloc(*out_defaults, sizeof(AST*) * (size_t)(*out_count + 1));
		(*out_names)[*out_count] = name;
		(*out_types)[*out_count] = type;
		(*out_defaults)[*out_count] = def;
		(*out_count)++;

		skip_ws();
		if (match(",")) continue;
		break;
	}

	match(")");
}

static ParsedSignature parse_function_signature(void) {
	ParsedSignature sig = { 0 };
	int save = pos, save_line = cur_line, save_col = cur_col;

	skip_ws();
	if (!match(":")) return sig;

	skip_ws();
	if (starts_with("(")) {
		/* Shorthand: no explicit return type → leave NULL so OP_RETURN skips
		 * check */
		sig.ret_type = NULL;
	} else {
		sig.ret_type = parse_union_type();
		if (!sig.ret_type) {
			pos = save;
			cur_line = save_line;
			cur_col = save_col;
			return sig;
		}
		skip_ws();
		if (!starts_with("(")) {
			pos = save;
			cur_line = save_line;
			cur_col = save_col;
			return sig;
		}
	}

	sig.is_function = 1;
	sig.param_types = parse_type_arg_list(&sig.param_count);
	sig.param_names = NULL;
	sig.param_defaults = NULL;
	return sig;
}

static AST* parse_lambda_body(char** param_names, Type** param_types,
							  AST** param_defaults, int param_count,
							  Type* ret_type) {
	int lline = cur_line, lcol = cur_col;
	if (!match("{")) return NULL;

	AST** body = NULL;
	int body_count = 0;
	block_depth++;
	while (1) {
		skip_ws();
		if (pos >= length || src[pos] == '}') break;
		AST* stmt = parse_statement();
		if (!stmt) {
			sync_to_next_statement();
			if (pos >= length || src[pos] == '}') break;
			continue;
		}
		body = realloc(body, sizeof(AST*) * (size_t)(body_count + 1));
		body[body_count++] = stmt;
	}
	block_depth--;
	match("}");

	AST* lambda = make_ast(AST_LAMBDA);
	lambda->u.lambda.ret_type = ret_type;
	lambda->u.lambda.param_names = param_names;
	lambda->u.lambda.param_types = param_types;
	lambda->u.lambda.param_defaults = param_defaults;
	lambda->u.lambda.param_count = param_count;
	lambda->u.lambda.body = body;
	lambda->u.lambda.body_count = body_count;
	lambda->line = lline;
	lambda->col = lcol;
	return lambda;
}

static AST* parse_lambda(void) {
	int save = pos, save_line = cur_line, save_col = cur_col;
	char** param_names = NULL;
	Type** param_types = NULL;
	AST** param_defaults = NULL;
	int param_count = 0;

	parse_param_list(&param_names, &param_types, &param_defaults, &param_count);
	skip_ws();
	if (!starts_with("{")) {
		pos = save;
		cur_line = save_line;
		cur_col = save_col;
		return NULL;
	}
	return parse_lambda_body(
		param_names, param_types, param_defaults, param_count, NULL);
}

static AST* parse_object_literal(void) {
	if (!match("{")) return NULL;

	char** names = NULL;
	AST** values = NULL;
	int field_count = 0;

	skip_ws();
	while (pos < length && !starts_with("}")) {
		char* name = parse_identifier();
		if (!name) break;
		skip_ws();

		AST* value = NULL;

		if (starts_with(",") || starts_with("}")) {
			/* Shorthand: func  →  var_ref("func") */
			AST* ref = make_ast(AST_VAR_REF);
			ref->u.var_ref = strdup(name);
			value = ref;
		} else if (starts_with("=") && !starts_with("==")) {
			/* foo = expr */
			match("=");
			skip_ws();
			value = parse_expression();
		} else if (starts_with(":")) {
			match(":");
			skip_ws();

			if (starts_with("(")) {
				/* zoo: (params) = { body }  —  implicit null return type */
				char** pnames = NULL;
				Type** ptypes = NULL;
				AST** pdefs = NULL;
				int pcount = 0;
				parse_param_list(&pnames, &ptypes, &pdefs, &pcount);
				skip_ws();
				if (starts_with("=") && !starts_with("==")) {
					match("=");
					skip_ws();
				}
				if (starts_with("{")) {
					value = parse_lambda_body(
						pnames, ptypes, pdefs, pcount, make_basic(BASIC_NULL));
				} else {
					value = parse_expression();
				}
			} else {
				/* bar: type = expr  OR  baz: ret_type (params) = { body } */
				Type* t = parse_union_type();
				skip_ws();
				if (t && starts_with("(")) {
					/* ret_type (params) = { body } */
					char** pnames = NULL;
					Type** ptypes = NULL;
					AST** pdefs = NULL;
					int pcount = 0;
					parse_param_list(&pnames, &ptypes, &pdefs, &pcount);
					skip_ws();
					if (starts_with("=") && !starts_with("==")) {
						match("=");
						skip_ws();
					}
					if (starts_with("{")) {
						value =
							parse_lambda_body(pnames, ptypes, pdefs, pcount, t);
					} else {
						value = parse_expression();
					}
				} else {
					/* name: type = expr */
					if (!t)
						parse_error(
							"Expected type after ':' in object field '%s'",
							name);
					if (starts_with("=") && !starts_with("==")) {
						match("=");
						skip_ws();
					}
					value = parse_expression();
				}
			}
		}

		if (!value) {
			free(name);
			break;
		}

		names = realloc(names, sizeof(char*) * (size_t)(field_count + 1));
		values = realloc(values, sizeof(AST*) * (size_t)(field_count + 1));
		names[field_count] = name;
		values[field_count] = value;
		field_count++;

		skip_ws();
		if (match(",")) {
			skip_ws();
			continue;
		}
	}

	match("}");

	AST* ast = make_ast(AST_OBJECT_LITERAL);
	ast->u.object_literal.names = names;
	ast->u.object_literal.values = values;
	ast->u.object_literal.field_count = field_count;
	return ast;
}

// NOTE: в языке не должно быть таких ограничений, кроме реально используемых
// слов
/* Table of unsupported keywords with suggestions. */
// static const struct {
// 	const char* kw;
// 	const char* hint;
// } unknown_kw_hints[] = {
// 	{ "let",		 "'let' is not a keyword — use 'var' or 'const'" }, 	{
// "function",
// 	  "'function' is not a keyword — declare with 'var name: type (params) = { "
// 	  "}'"																   },
// 	{ "def",
// 	  "'def' is not a keyword — declare with 'var name: type (params) = { }'" },
// 	{ "fn",
// 	  "'fn' is not a keyword — declare with 'var name: type (params) = { }'"
// }, 	{ "fun",
// 	  "'fun' is not a keyword — declare with 'var name: type (params) = { }'" },
// 	{ "class",	   "'class' is not a keyword — use 'interface'"				},
// 	{ "struct",	"'struct' is not a keyword — use 'interface'"			  },
// 	{ NULL,		NULL													   }
// };

static AST* parse_statement(void) {
	skip_ws();
	if (starts_with_keyword("if")) return parse_if();
	if (starts_with_keyword("while")) return parse_while();
	if (starts_with_keyword("do")) return parse_do_while();
	if (starts_with_keyword("for")) return parse_for();
	if (starts_with_keyword("switch")) return parse_switch();
	if (starts_with_keyword("break")) return parse_break();
	if (starts_with_keyword("continue")) return parse_continue();
	if (starts_with("{")) return parse_block();

	/* Detect common keywords from other languages before expression parsing,
	   so the error points to the right line/col. */
	// for (int i = 0; unknown_kw_hints[i].kw; i++) {
	// 	if (starts_with_keyword(unknown_kw_hints[i].kw)) {
	// 		parse_error("%s", unknown_kw_hints[i].hint);
	// 		return NULL;
	// 	}
	// }

	int expr_line = cur_line, expr_col = cur_col;
	AST* stmt = parse_expression();
	if (!stmt) {
		parse_error("Unexpected token '%c' in statement",
					pos < length ? src[pos] : '?');
		return NULL;
	}
	if (!is_valid_statement(stmt)) {
		char snippet[64];
		source_line_snippet(expr_line, expr_col, snippet, sizeof(snippet));
		cur_line = expr_line;
		cur_col = expr_col;
		parse_error("%s is not a valid statement: '%s'",
					ast_kind_name(stmt->kind),
					snippet);
		return NULL;
	}
	skip_ws();
	match(";");
	return stmt;
}

static AST* parse_primary(void) {
	skip_ws();
	if (pos >= length) return NULL;

	if (src[pos] == '{') return parse_object_literal();
	if (src[pos] == '[') {
		match("[");
		skip_ws();
		AST** elems = NULL;
		int count = 0;
		while (pos < length && src[pos] != ']') {
			AST* elem = parse_expression();
			if (!elem) break;
			elems = realloc(elems, sizeof(AST*) * (size_t)(count + 1));
			elems[count++] = elem;
			skip_ws();
			if (!match(",")) break;
			skip_ws();
		}
		match("]");
		AST* ast = make_ast(AST_ARRAY_LITERAL);
		ast->u.array_literal.values = elems;
		ast->u.array_literal.count = count;
		return ast;
	}
	if (src[pos] == '"') {
		return parse_interpolated_string();
	}
	if (src[pos] == '\'') {
		char* literal = parse_single_string_literal();
		AST* ast = make_literal_ast(make_basic(BASIC_STRING));
		ast->u.literal.val.s = literal;
		return ast;
	}
	if (starts_with_keyword("true")) {
		pos += 4;
		AST* ast = make_literal_ast(make_basic(BASIC_BOOL));
		ast->u.literal.val.b = 1;
		return ast;
	}
	if (starts_with_keyword("false")) {
		pos += 5;
		AST* ast = make_literal_ast(make_basic(BASIC_BOOL));
		ast->u.literal.val.b = 0;
		return ast;
	}
	if (starts_with_keyword("null")) {
		pos += 4;
		return make_literal_ast(make_basic(BASIC_NULL));
	}
	if (isdigit((unsigned char)src[pos])) return parse_number_literal();
	if (src[pos] == '(') {
		int save = pos, save_line = cur_line, save_col = cur_col;
		AST* lambda = parse_lambda();
		if (lambda) return lambda;
		pos = save;
		cur_line = save_line;
		cur_col = save_col;
		if (match("(")) {
			AST* expr = parse_expression();
			match(")");
			return expr;
		}
	}

	skip_ws();
	/* Try typed lambda: TypeName(params) = { body } */
	{
		int save = pos, save_line = cur_line, save_col = cur_col;
		/* Peek to avoid parse_error for unknown identifiers in speculative
		 * parse */
		skip_ws();
		char* peek = parse_identifier();
		int is_type = peek && is_known_type_name(peek);
		free(peek);
		pos = save;
		cur_line = save_line;
		cur_col = save_col;
		if (!is_type) goto skip_typed_lambda;
		Type* t = parse_named_or_basic_type();
		skip_ws();
		if (t && pos < length && src[pos] == '(') {
			char** pnames = NULL;
			Type** ptypes = NULL;
			AST** pdefs = NULL;
			int pcount = 0;
			parse_param_list(&pnames, &ptypes, &pdefs, &pcount);
			skip_ws();
			if (match("=>")
				|| (starts_with("=") && !starts_with("==") && match("="))) {
				skip_ws();
				if (starts_with("{")) {
					return parse_lambda_body(pnames, ptypes, pdefs, pcount, t);
				}
			}
		}
		pos = save;
		cur_line = save_line;
		cur_col = save_col;
	}
skip_typed_lambda:;
	int id_line = cur_line, id_col = cur_col;
	char* id = parse_identifier();
	if (!id) return NULL;
	AST* ref = malloc(sizeof(AST));
	ref->kind = AST_VAR_REF;
	ref->line = id_line;
	ref->col = id_col;
	ref->u.var_ref = id;
	return ref;
}

static AST* parse_postfix(void) {
	AST* expr = parse_primary();
	if (!expr) return NULL;

	while (1) {
		skip_ws();
		if (match(".")) {
			char* member = parse_identifier();
			AST* ast = malloc(sizeof(AST));
			ast->kind = AST_MEMBER_ACCESS;
			ast->line = expr->line;
			ast->col = expr->col;
			ast->u.member_access.object = expr;
			ast->u.member_access.member = member;
			expr = ast;
			continue;
		}

		if (match("(")) {
			AST** args = NULL;
			int arg_count = 0;
			if (!match(")")) {
				while (1) {
					AST* arg = parse_expression();
					if (!arg) break;
					args =
						realloc(args, sizeof(AST*) * (size_t)(arg_count + 1));
					args[arg_count++] = arg;
					skip_ws();
					if (match(")")) break;
					match(",");
				}
			}
			AST* call = malloc(sizeof(AST));
			call->kind = AST_CALL;
			call->line = expr->line;
			call->col = expr->col;
			call->u.call.func = expr;
			call->u.call.args = args;
			call->u.call.arg_count = arg_count;
			expr = call;
			continue;
		}

		if (match("[")) {
			AST* idx = parse_expression();
			match("]");
			AST* ast = make_ast(AST_INDEX_ACCESS);
			ast->u.index_access.object = expr;
			ast->u.index_access.index = idx;
			expr = ast;
			continue;
		}

		if (match("++")) {
			AST* ast = malloc(sizeof(AST));
			ast->kind = AST_UNARY;
			ast->u.unary.op = UNARY_POST_INC;
			ast->u.unary.operand = expr;
			expr = ast;
			continue;
		}

		if (match("--")) {
			AST* ast = malloc(sizeof(AST));
			ast->kind = AST_UNARY;
			ast->u.unary.op = UNARY_POST_DEC;
			ast->u.unary.operand = expr;
			expr = ast;
			continue;
		}

		break;
	}

	return expr;
}

static AST* parse_unary(void) {
	skip_ws();
	if (match("++")) {
		AST* operand = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_UNARY;
		ast->u.unary.op = UNARY_PRE_INC;
		ast->u.unary.operand = operand;
		return ast;
	}
	if (match("--")) {
		AST* operand = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_UNARY;
		ast->u.unary.op = UNARY_PRE_DEC;
		ast->u.unary.operand = operand;
		return ast;
	}
	if (match("+")) {
		return parse_unary();
	}
	if (match("-")) {
		AST* operand = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_UNARY;
		ast->u.unary.op = '-';
		ast->u.unary.operand = operand;
		return ast;
	}
	if (match("!")) {
		AST* operand = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_UNARY;
		ast->u.unary.op = '!';
		ast->u.unary.operand = operand;
		return ast;
	}
	if (match("~")) {
		AST* operand = parse_unary();
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_UNARY;
		ast->u.unary.op = UNARY_BIT_NOT;
		ast->u.unary.operand = operand;
		return ast;
	}
	return parse_postfix();
}

// static AST* parse_subtraction(void) {
// 	AST* left = parse_unary();
// 	if (!left) return NULL;

// 	while (1) {
// 		skip_ws();
// 		if (!match("-")) break;
// 		AST* right = parse_postfix();
// 		AST* ast = malloc(sizeof(AST));
// 		ast->kind = AST_BINARY;
// 		ast->u.binary.op = '-';
// 		ast->u.binary.left = left;
// 		ast->u.binary.right = right;
// 		left = ast;
// 	}

// 	return left;
// }

static Type* infer_decl_type(AST* init) {
	if (!init) return NULL;
	switch (init->kind) {
		case AST_LAMBDA:
			return make_func(init->u.lambda.ret_type,
							 init->u.lambda.param_types,
							 init->u.lambda.param_count);
		case AST_LITERAL:
			return init->u.literal.type;
		case AST_STRING_INTERP:
			return make_basic(BASIC_STRING);
		default:
			return NULL;
	}
}

static AST* parse_enum_decl(void) {
	if (!match("enum")) return NULL;
	char* name = parse_identifier();
	if (!name) {
		parse_error("Expected enum name");
		return NULL;
	}

	char** member_names = NULL;
	AST** member_values = NULL;
	int member_count = 0;

	if (!match("{")) {
		parse_error("Expected '{' after enum name");
		free(name);
		return NULL;
	}
	skip_ws();

	while (pos < length && !starts_with("}")) {
		skip_ws();
		if (starts_with("}")) break;
		char* mname = parse_identifier();
		if (!mname) break;
		skip_ws();

		AST* mval = NULL;
		if (starts_with("=") && !starts_with("==")) {
			pos++;
			skip_ws();
			mval = parse_conditional();
		}

		member_names =
			realloc(member_names, sizeof(char*) * (size_t)(member_count + 1));
		member_values =
			realloc(member_values, sizeof(AST*) * (size_t)(member_count + 1));
		member_names[member_count] = mname;
		member_values[member_count] = mval;
		member_count++;

		skip_ws();
		match(",");
		skip_ws();
	}
	match("}");

	register_named_type(name, make_basic(BASIC_ANY));

	AST* ast = make_ast(AST_ENUM_DECL);
	ast->u.enum_decl.name = name;
	ast->u.enum_decl.member_names = member_names;
	ast->u.enum_decl.member_values = member_values;
	ast->u.enum_decl.member_count = member_count;
	return ast;
}

static AST* parse_interface_decl(void) {
	if (!match("interface")) return NULL;
	char* name = parse_identifier();
	InterfaceField* fields = NULL;
	int field_count = 0;

	// Pre-register with empty fields so self-referential types (e.g. `this: IAnimal`) resolve
	Type* iface_type = make_interface(name, NULL, 0);
	register_named_type(name, iface_type);

	match("{");
	skip_ws();
	while (pos < length && !starts_with("}")) {
		char* field_name = parse_identifier();
		Type* field_type = NULL;
		if (!field_name) break;
		match(":");
		field_type = parse_type();
		fields =
			realloc(fields, sizeof(InterfaceField) * (size_t)(field_count + 1));
		fields[field_count].name = field_name;
		fields[field_count].type = field_type;
		field_count++;
		skip_ws();
		match(",");
		skip_ws();
	}
	match("}");

	iface_type->u.iface.fields = fields;
	iface_type->u.iface.field_count = field_count;

	AST* ast = malloc(sizeof(AST));
	ast->kind = AST_INTERFACE_DECL;
	ast->u.interface_decl.name = strdup(name);
	ast->u.interface_decl.iface_type = iface_type;
	free(name);
	return ast;
}

static AST* parse_import_decl(void) {
	skip_ws();
	int save_line = cur_line, save_col = cur_col;
	if (!match("import")) return NULL;
	if (block_depth > 0) {
		parse_error("'import' is only allowed at the top level of a module");
		return NULL;
	}

	int name_count = 0;
	char** names = parse_name_list(&name_count);
	char* source = NULL;

	skip_ws();
	match("from");
	skip_ws();
	source = parse_string_literal();

	AST* ast = make_ast(AST_IMPORT_DECL);
	ast->line = save_line;
	ast->col = save_col;
	ast->u.import_decl.names = names;
	ast->u.import_decl.name_count = name_count;
	ast->u.import_decl.source = source;
	return ast;
}

static AST* parse_export_decl(void) {
	skip_ws();
	int save_line = cur_line, save_col = cur_col;
	if (!match("export")) return NULL;
	if (block_depth > 0) {
		parse_error("'export' is only allowed at the top level of a module");
		return NULL;
	}

	int name_count = 0;
	char** names = parse_name_list(&name_count);

	AST* ast = make_ast(AST_EXPORT_DECL);
	ast->line = save_line;
	ast->col = save_col;
	ast->u.export_decl.names = names;
	ast->u.export_decl.name_count = name_count;
	return ast;
}

static AST* parse_expression(void) {
	skip_ws();

	if (starts_with_keyword("import")) return parse_import_decl();
	if (starts_with_keyword("export")) return parse_export_decl();
	if (starts_with_keyword("interface")) return parse_interface_decl();
	if (starts_with_keyword("enum")) return parse_enum_decl();

	if (starts_with_keyword("return")) {
		pos += 6;
		AST* ast = malloc(sizeof(AST));
		ast->kind = AST_RETURN;
		ast->u.return_stmt.expr = parse_expression();
		return ast;
	}

	if (starts_with_keyword("var") || starts_with_keyword("const")) {
		int is_const = starts_with_keyword("const");
		int decl_line = cur_line, decl_col = cur_col;
		pos += is_const ? 5 : 3;
		skip_ws();
		char* name = parse_identifier();
		Type* declared_type = NULL;
		ParsedSignature sig = { 0 };
		AST* init = NULL;

		skip_ws();
		if (starts_with(":")) {
			int save = pos, save_line = cur_line, save_col = cur_col;
			sig = parse_function_signature();
			if (sig.is_function) {
				declared_type =
					make_func(sig.ret_type, sig.param_types, sig.param_count);
			} else {
				pos = save;
				cur_line = save_line;
				cur_col = save_col;
				match(":");
				declared_type = parse_type();
			}
		}

		if (!match("=")) {
			parse_error("expected '=' in declaration of '%s'", name);
			return NULL;
		}
		skip_ws();

		if (sig.is_function && starts_with("{")) {
			parse_error(
				"expected parameter list before '{' in function declaration "
				"'%s' — use '= (params) { body }' or '= () { body }'",
				name);
			return NULL;
		}

		if (sig.is_function && starts_with("(")) {
			int save2 = pos, save_line2 = cur_line, save_col2 = cur_col;
			char** pnames = NULL;
			Type** ptypes = NULL;
			AST** pdefs = NULL;
			int pcount = 0;
			parse_param_list(&pnames, &ptypes, &pdefs, &pcount);
			skip_ws();
			if (starts_with("{")) {
				init = parse_lambda_body(pnames, ptypes, pdefs, pcount,
										 sig.ret_type);
			} else {
				pos = save2;
				cur_line = save_line2;
				cur_col = save_col2;
				init = parse_conditional();
			}
		} else {
			init = parse_conditional();
		}

		AST* decl = malloc(sizeof(AST));
		decl->kind = AST_VAR_DECL;
		decl->line = decl_line;
		decl->col = decl_col;
		decl->u.var_decl.name = name;
		decl->u.var_decl.vartype =
			declared_type ? declared_type : infer_decl_type(init);
		decl->u.var_decl.init = init;
		decl->u.var_decl.is_const = is_const;
		return decl;
	}

	return parse_assign();
}

AST* parse_program(const char* code) {
	src = code;
	pos = 0;
	length = (int)strlen(src);
	cur_line = 1;
	cur_col = 1;
	named_types = NULL;
	named_type_count = 0;
	parse_failed = 0;
	error_count = 0;
	block_depth = 0;

	AST** body = NULL;
	int body_count = 0;
	while (1) {
		skip_ws();
		if (pos >= length) break;
		AST* stmt = parse_statement();
		if (!stmt) {
			if (pos >= length) break;
			sync_to_next_line();
			if (pos >= length) break;
			continue;
		}
		body = realloc(body, sizeof(AST*) * (size_t)(body_count + 1));
		body[body_count++] = stmt;
	}

	if (error_count > 0) return NULL;

	/* Duplicate import/export name detection */
	char** seen_imports = NULL;
	int seen_import_count = 0;
	char** seen_exports = NULL;
	int seen_export_count = 0;

	for (int i = 0; i < body_count; i++) {
		AST* s = body[i];
		if (s->kind == AST_IMPORT_DECL) { /* import_decl */
			for (int j = 0; j < s->u.import_decl.name_count; j++) {
				char* name = s->u.import_decl.names[j];
				int is_dup = 0;
				for (int k = 0; k < seen_import_count; k++) {
					if (strcmp(seen_imports[k], name) == 0) {
						cur_line = s->line;
						cur_col = s->col;
						parse_error("Duplicate import '%s'", name);
						is_dup = 1;
						break;
					}
				}
				if (!is_dup) {
					seen_imports = realloc(
						seen_imports,
						sizeof(char*) * (size_t)(seen_import_count + 1));
					seen_imports[seen_import_count++] = name;
				}
			}
		} else if (s->kind == AST_EXPORT_DECL) { /* export_decl */
			for (int j = 0; j < s->u.export_decl.name_count; j++) {
				char* name = s->u.export_decl.names[j];
				int is_dup = 0;
				for (int k = 0; k < seen_export_count; k++) {
					if (strcmp(seen_exports[k], name) == 0) {
						cur_line = s->line;
						cur_col = s->col;
						parse_error("Duplicate export '%s'", name);
						is_dup = 1;
						break;
					}
				}
				if (!is_dup) {
					seen_exports = realloc(
						seen_exports,
						sizeof(char*) * (size_t)(seen_export_count + 1));
					seen_exports[seen_export_count++] = name;
				}
			}
		}
	}
	free(seen_imports);
	free(seen_exports);

	if (error_count > 0) return NULL;

	AST* program = make_ast(AST_PROGRAM);
	program->u.program.body = body;
	program->u.program.body_count = body_count;
	return program;
}
