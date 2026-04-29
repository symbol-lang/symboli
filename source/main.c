#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "builtins.h"
#include "eval.h"
#include "parser.h"
#include "types.h"
#include "vm.h"

typedef struct ModuleCache {
	char* path;
	Env* exports;
	struct ModuleCache* next;
} ModuleCache;

static char* dup_cstr(const char* s) {
	size_t len = strlen(s);
	char* copy = malloc(len + 1);
	memcpy(copy, s, len + 1);
	return copy;
}

/* Global command line arguments for builtins */
int global_argc;
char** global_argv;

static void print_ast_json(AST* node);

static const char* kind_name(ASTKind k) {
	switch (k) {
		case AST_VAR_DECL: return "var_decl";
		case AST_LAMBDA: return "lambda";
		case AST_CALL: return "call";
		case AST_LITERAL: return "literal";
		case AST_VAR_REF: return "var_ref";
		case AST_STRING_INTERP: return "string_interp";
		case AST_PROGRAM: return "program";
		case AST_INTERFACE_DECL: return "interface_decl";
		case AST_OBJECT_LITERAL: return "object_literal";
		case AST_MEMBER_ACCESS: return "member_access";
		case AST_MEMBER_ASSIGN: return "member_assign";
		case AST_RETURN: return "return";
		case AST_BINARY: return "binary";
		case AST_IMPORT_DECL: return "import";
		case AST_EXPORT_DECL: return "export";
		case AST_IF: return "if";
		case AST_WHILE: return "while";
		case AST_DO_WHILE: return "do_while";
		case AST_FOR: return "for";
		case AST_SWITCH: return "switch";
		case AST_BREAK: return "break";
		case AST_CONTINUE: return "continue";
		case AST_ASSIGN: return "assign";
		case AST_UNARY: return "unary";
		case AST_CONDITIONAL: return "conditional";
		case AST_ARRAY_LITERAL: return "array_literal";
		case AST_INDEX_ACCESS: return "index_access";
		case AST_INDEX_ASSIGN: return "index_assign";
		case AST_ENUM_DECL: return "enum_decl";
		default: return "unknown";
	}
}

static void print_ast_json(AST* node) {
	if (!node) {
		printf("null");
		return;
	}

	printf("{\"kind\":\"%s\",\"line\":%d,\"col\":%d",
		   kind_name(node->kind), node->line, node->col);

	switch (node->kind) {
		case AST_PROGRAM:
			printf(",\"body\":[");
			for (int i = 0; i < node->u.program.body_count; i++) {
				if (i > 0) printf(",");
				print_ast_json(node->u.program.body[i]);
			}
			printf("]");
			break;

		case AST_VAR_DECL:
			printf(",\"name\":\"%s\"", node->u.var_decl.name);
			if (node->u.var_decl.vartype) {
				char* ts = type_to_string(node->u.var_decl.vartype);
				printf(",\"vartype\":\"%s\"", ts);
				free(ts);
			}
			if (node->u.var_decl.init) {
				printf(",\"init\":");
				print_ast_json(node->u.var_decl.init);
			}
			break;

		case AST_LITERAL:
			printf(",\"value\":");
			if (node->u.literal.type->kind == TYPE_BASIC) {
				switch (node->u.literal.type->u.basic) {
					case BASIC_INT:
						printf("%d", node->u.literal.val.i);
						break;
					case BASIC_FLOAT:
						printf("%g", node->u.literal.val.f);
						break;
					case BASIC_STRING:
						printf("\"");
						for (const char* p = node->u.literal.val.s; p && *p; p++) {
							switch (*p) {
								case '"':  printf("\\\""); break;
								case '\\': printf("\\\\"); break;
								case '\n': printf("\\n");  break;
								case '\r': printf("\\r");  break;
								case '\t': printf("\\t");  break;
								default:   printf("%c", *p); break;
							}
						}
						printf("\"");
						break;
					case BASIC_BOOL:
						printf("%s", node->u.literal.val.b ? "true" : "false");
						break;
					default:
						printf("null");
				}
			} else {
				printf("null");
			}
			break;

		case AST_VAR_REF:
			printf(",\"name\":\"%s\"", node->u.var_ref);
			break;

		case AST_CALL:
			printf(",\"func\":");
			print_ast_json(node->u.call.func);
			printf(",\"args\":[");
			for (int i = 0; i < node->u.call.arg_count; i++) {
				if (i > 0) printf(",");
				print_ast_json(node->u.call.args[i]);
			}
			printf("]");
			break;

		case AST_BINARY:
			printf(",\"op\":%d,\"left\":", node->u.binary.op);
			print_ast_json(node->u.binary.left);
			printf(",\"right\":");
			print_ast_json(node->u.binary.right);
			break;

		case AST_UNARY:
			printf(",\"op\":%d,\"operand\":", node->u.unary.op);
			print_ast_json(node->u.unary.operand);
			break;

		case AST_IF:
			printf(",\"cond\":");
			print_ast_json(node->u.if_stmt.cond);
			printf(",\"then\":");
			print_ast_json(node->u.if_stmt.then_branch);
			if (node->u.if_stmt.else_branch) {
				printf(",\"else\":");
				print_ast_json(node->u.if_stmt.else_branch);
			}
			break;

		case AST_WHILE:
			printf(",\"cond\":");
			print_ast_json(node->u.while_stmt.cond);
			printf(",\"body\":");
			print_ast_json(node->u.while_stmt.body);
			break;

		case AST_FOR:
			printf(",\"init\":");
			print_ast_json(node->u.for_stmt.init);
			printf(",\"cond\":");
			print_ast_json(node->u.for_stmt.cond);
			printf(",\"update\":");
			print_ast_json(node->u.for_stmt.update);
			printf(",\"body\":");
			print_ast_json(node->u.for_stmt.body);
			break;

		case AST_RETURN:
			if (node->u.return_stmt.expr) {
				printf(",\"expr\":");
				print_ast_json(node->u.return_stmt.expr);
			}
			break;

		case AST_LAMBDA:
			if (node->u.lambda.ret_type) {
				char* ts = type_to_string(node->u.lambda.ret_type);
				printf(",\"ret_type\":\"%s\"", ts);
				free(ts);
			}
			if (node->u.lambda.is_variadic)
				printf(",\"is_variadic\":1");
			printf(",\"params\":[");
			for (int i = 0; i < node->u.lambda.param_count; i++) {
				if (i > 0) printf(",");
				int is_var_param = node->u.lambda.is_variadic
				                   && i + 1 == node->u.lambda.param_count;
				printf("{\"name\":\"%s%s\"",
				       is_var_param ? "..." : "",
				       node->u.lambda.param_names[i]);
				if (node->u.lambda.param_types && node->u.lambda.param_types[i]) {
					char* ts = type_to_string(node->u.lambda.param_types[i]);
					printf(",\"type\":\"%s\"", ts);
					free(ts);
				}
				printf("}");
			}
			printf("],\"body\":[");
			for (int i = 0; i < node->u.lambda.body_count; i++) {
				if (i > 0) printf(",");
				print_ast_json(node->u.lambda.body[i]);
			}
			printf("]");
			break;

		default:
			break;
	}

	printf("}");
}

static char* read_file(const char* filename) {
	FILE* f = fopen(filename, "r");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = malloc((size_t)size + 1);
	fread(buf, 1, (size_t)size, f);
	buf[size] = '\0';
	fclose(f);
	return buf;
}

static char* dirname_of(const char* path) {
	const char* slash = strrchr(path, '/');
	if (!slash) return dup_cstr(".");
	size_t len = (size_t)(slash - path);
	char* dir = malloc(len + 1);
	memcpy(dir, path, len);
	dir[len] = '\0';
	return dir;
}

static char* join_path(const char* left, const char* right) {
	size_t left_len = strlen(left);
	size_t right_len = strlen(right);
	char* path = malloc(left_len + right_len + 2);
	memcpy(path, left, left_len);
	path[left_len] = '/';
	memcpy(path + left_len + 1, right, right_len + 1);
	return path;
}

static ModuleCache* find_module(ModuleCache* cache, const char* path) {
	for (ModuleCache* entry = cache; entry; entry = entry->next) {
		if (strcmp(entry->path, path) == 0) return entry;
	}
	return NULL;
}

static int collect_exports(AST* program, Env* env, Env** out_exports) {
	Env* exports = NULL;

	for (int i = 0; i < program->u.program.body_count; i++) {
		AST* stmt = program->u.program.body[i];
		if (stmt->kind != AST_EXPORT_DECL) continue;

		if (stmt->u.export_decl.name_count == 0) {
			fprintf(stderr,
					"Syntax error at line %d, column %d: empty export\n",
					stmt->line, stmt->col);
			return 0;
		}

		for (int j = 0; j < stmt->u.export_decl.name_count; j++) {
			const char* name = stmt->u.export_decl.names[j];
			Value* value = env_get(env, (char*)name);
			if (!value) {
				fprintf(stderr,
						"Syntax error at line %d, column %d: cannot export undefined '%s'\n",
						stmt->line, stmt->col, name);
				return 0;
			}
			exports = env_add(exports, (char*)name, value);
		}
	}

	*out_exports = exports;
	return 1;
}

static int execute_module(const char* module_path, ModuleCache** cache,
						  Env** out_exports) {
	if (strcmp(module_path, "symbol") == 0) {
		*out_exports = make_builtin_exports();
		return 1;
	}

	ModuleCache* cached = find_module(*cache, module_path);
	if (cached) {
		*out_exports = cached->exports;
		return 1;
	}

	char* code = read_file(module_path);
	if (!code) {
		fprintf(stderr, "Cannot read file %s\n", module_path);
		return 0;
	}

	AST* program = parse_program(code);
	if (!program) {
		fprintf(stderr, "Failed to parse source %s\n", module_path);
		free(code);
		return 0;
	}

	eval_set_filename(module_path);

	Env* env = make_builtin_exports();
	env = env_push_scope(env);
	char* module_dir = dirname_of(module_path);

	for (int i = 0; i < program->u.program.body_count; i++) {
		AST* stmt = program->u.program.body[i];

		if (stmt->kind == AST_IMPORT_DECL) {
			Env* imported_exports = NULL;
			char* resolved_path = NULL;

			if (strcmp(stmt->u.import_decl.source, "symbol") == 0)
				resolved_path = dup_cstr("symbol");
			else
				resolved_path =
					join_path(module_dir, stmt->u.import_decl.source);

			if (!execute_module(resolved_path, cache, &imported_exports)) {
				free(resolved_path);
				free(module_dir);
				free(code);
				return 0;
			}
			free(resolved_path);
			eval_set_filename(module_path);

			for (int j = 0; j < stmt->u.import_decl.name_count; j++) {
				const char* name = stmt->u.import_decl.names[j];
				Value* imported = env_get(imported_exports, (char*)name);
				if (!imported) {
					fprintf(stderr,
							"Runtime error: module '%s' does not export '%s'\n",
							stmt->u.import_decl.source,
							name);
					free(module_dir);
					free(code);
					return 0;
				}
				env = env_add(env, (char*)name, imported);
			}

			continue;
		}

		if (stmt->kind == AST_EXPORT_DECL) continue;

		if (!eval(stmt, &env)) {
			free(module_dir);
			free(code);
			return 0;
		}
	}

	Env* exports = NULL;
	if (!collect_exports(program, env, &exports)) {
		free(module_dir);
		free(code);
		return 0;
	}

	ModuleCache* entry = malloc(sizeof(ModuleCache));
	entry->path = dup_cstr(module_path);
	entry->exports = exports;
	entry->next = *cache;
	*cache = entry;

	*out_exports = exports;
	free(module_dir);
	free(code);
	return 1;
}

static void print_help(const char* prog) {
	printf("Usage: %s [options] <file>\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help     Show this help message\n");
	printf("  -d, --disasm   Disassemble bytecode instead of running\n");
	printf("  -a, --ast      Print abstract syntax tree as JSON\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s script.sym\n", prog);
	printf("  %s --disasm script.sym\n", prog);
	printf("  %s --ast script.sym\n", prog);
}

int main(int argc, char* argv[]) {
	setlocale(LC_ALL, "");

	global_argc = argc;
	global_argv = argv;

	int disasm_mode = 0;
	int ast_mode = 0;
	const char* filename = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_help(argv[0]);
			return 0;
		} else if (strcmp(argv[i], "--disasm") == 0 ||
				   strcmp(argv[i], "-d") == 0)
			disasm_mode = 1;
		else if (strcmp(argv[i], "--ast") == 0 ||
				   strcmp(argv[i], "-a") == 0)
			ast_mode = 1;
		else if (!filename)
			filename = argv[i];
	}

	if (!filename) {
		print_help(argv[0]);
		return 1;
	}

	if (disasm_mode) {
		char* code = read_file(filename);
		if (!code) {
			fprintf(stderr, "Cannot read file %s\n", filename);
			return 1;
		}
		AST* program = parse_program(code);
		if (!program) {
			fprintf(stderr, "Failed to parse source %s\n", filename);
			free(code);
			return 1;
		}
		Chunk* ch = compile(program, filename);
		chunk_disassemble(ch, filename, 0);
		chunk_free(ch);
		free(code);
		return 0;
	}

	if (ast_mode) {
		char* code = read_file(filename);
		if (!code) {
			fprintf(stderr, "Cannot read file %s\n", filename);
			return 1;
		}
		AST* program = parse_program(code);
		if (!program) {
			fprintf(stderr, "Failed to parse source %s\n", filename);
			free(code);
			return 1;
		}
		print_ast_json(program);
		printf("\n");
		free(code);
		return 0;
	}

	ModuleCache* cache = NULL;
	Env* exports = NULL;
	if (!execute_module(filename, &cache, &exports)) return 1;
	return 0;
}
