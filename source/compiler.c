#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Chunk management ────────────────────────────────────────── */

Chunk* chunk_new(void) {
	return calloc(1, sizeof(Chunk));
}

static void chunk_emit(Chunk* ch, uint8_t op, int32_t a, int32_t b, int line,
					   int col) {
	if (ch->len == ch->cap) {
		ch->cap = ch->cap ? ch->cap * 2 : 8;
		ch->code = realloc(ch->code, (size_t)ch->cap * sizeof(Instr));
		ch->lines = realloc(ch->lines, (size_t)ch->cap * sizeof(int));
		ch->cols = realloc(ch->cols, (size_t)ch->cap * sizeof(int));
	}
	ch->code[ch->len] = (Instr){ op, a, b };
	ch->lines[ch->len] = line;
	ch->cols[ch->len] = col;
	ch->len++;
}

static int chunk_add_const(Chunk* ch, Value* v) {
	if (ch->nconst == ch->constcap) {
		ch->constcap = ch->constcap ? ch->constcap * 2 : 8;
		ch->consts = realloc(ch->consts, (size_t)ch->constcap * sizeof(Value*));
	}
	ch->consts[ch->nconst++] = v;
	return ch->nconst - 1;
}

/* Deduplicated name pool — variable names, member names. */
static int chunk_add_name(Chunk* ch, const char* name) {
	for (int i = 0; i < ch->nname; i++)
		if (strcmp(ch->names[i], name) == 0) return i;
	if (ch->nname == ch->namecap) {
		ch->namecap = ch->namecap ? ch->namecap * 2 : 8;
		ch->names = realloc(ch->names, (size_t)ch->namecap * sizeof(char*));
	}
	ch->names[ch->nname++] = strdup(name);
	return ch->nname - 1;
}

static int chunk_add_sub(Chunk* ch, Chunk* sub) {
	if (ch->nsub == ch->subcap) {
		ch->subcap = ch->subcap ? ch->subcap * 2 : 4;
		ch->subs = realloc(ch->subs, (size_t)ch->subcap * sizeof(Chunk*));
	}
	ch->subs[ch->nsub++] = sub;
	return ch->nsub - 1;
}

static int chunk_add_interp(Chunk* ch, InterpEntry e) {
	if (ch->ninterp == ch->interpcap) {
		ch->interpcap = ch->interpcap ? ch->interpcap * 2 : 4;
		ch->interps =
			realloc(ch->interps, (size_t)ch->interpcap * sizeof(InterpEntry));
	}
	ch->interps[ch->ninterp++] = e;
	return ch->ninterp - 1;
}

static int chunk_add_type(Chunk* ch, Type* t) {
	if (ch->ntype == ch->typecap) {
		ch->typecap = ch->typecap ? ch->typecap * 2 : 4;
		ch->type_pool =
			realloc(ch->type_pool, (size_t)ch->typecap * sizeof(Type*));
	}
	ch->type_pool[ch->ntype++] = t;
	return ch->ntype - 1;
}

static int chunk_add_objtmpl(Chunk* ch, ObjTemplate t) {
	if (ch->nobjt == ch->objtcap) {
		ch->objtcap = ch->objtcap ? ch->objtcap * 2 : 4;
		ch->objtmpls =
			realloc(ch->objtmpls, (size_t)ch->objtcap * sizeof(ObjTemplate));
	}
	ch->objtmpls[ch->nobjt++] = t;
	return ch->nobjt - 1;
}

void chunk_free(Chunk* ch) {
	if (!ch) return;
	free(ch->code);
	free(ch->lines);
	free(ch->cols);
	free(ch->consts);
	for (int i = 0; i < ch->nname; i++)
		free(ch->names[i]);
	free(ch->names);
	for (int i = 0; i < ch->nsub; i++)
		chunk_free(ch->subs[i]);
	free(ch->subs);
	for (int i = 0; i < ch->ninterp; i++)
		free(ch->interps[i].tmpl);
	free(ch->interps);
	for (int i = 0; i < ch->nobjt; i++) {
		for (int j = 0; j < ch->objtmpls[i].count; j++)
			free(ch->objtmpls[i].names[j]);
		free(ch->objtmpls[i].names);
	}
	free(ch->objtmpls);
	free(ch->type_pool); /* pointers borrowed from AST, not owned */
	if (ch->param_names) {
		for (int i = 0; i < ch->param_count; i++)
			free(ch->param_names[i]);
		free(ch->param_names);
	}
	free(ch->param_types);
	if (ch->param_defaults) {
		for (int i = 0; i < ch->param_count; i++)
			chunk_free(ch->param_defaults[i]);
		free(ch->param_defaults);
	}
	free(ch->name);
	free(ch);
}

/* ── Compiler ────────────────────────────────────────────────── */

#define MAX_BREAK_DEPTH	  64
#define MAX_BREAK_PATCHES 512

typedef struct {
	Chunk* chunk;
	const char* filename;
	int break_patches[MAX_BREAK_DEPTH][MAX_BREAK_PATCHES];
	int break_counts[MAX_BREAK_DEPTH];
	int break_depth;
	int continue_patches[MAX_BREAK_DEPTH][MAX_BREAK_PATCHES];
	int continue_counts[MAX_BREAK_DEPTH];
	int continue_depth;
} Compiler;

static void compiler_init(Compiler* c, Chunk* ch, const char* filename) {
	memset(c, 0, sizeof(Compiler));
	c->chunk = ch;
	c->filename = filename;
}

static void push_break_scope(Compiler* c) {
	if (c->break_depth < MAX_BREAK_DEPTH) {
		c->break_counts[c->break_depth] = 0;
		c->break_depth++;
	}
}

static void pop_break_scope(Compiler* c, int target) {
	if (c->break_depth == 0) return;
	c->break_depth--;
	int d = c->break_depth;
	for (int i = 0; i < c->break_counts[d]; i++) {
		int p = c->break_patches[d][i];
		c->chunk->code[p].a = target - p - 1;
	}
}

static void emit_break_jump(Compiler* c, int line, int col) {
	if (c->break_depth == 0) {
		fprintf(stderr,
				"%s: break outside loop/switch\n",
				c->filename ? c->filename : "");
		return;
	}
	int patch_ip = c->chunk->len;
	chunk_emit(c->chunk, OP_JUMP, 0, 0, line, col);
	int d = c->break_depth - 1;
	if (c->break_counts[d] < MAX_BREAK_PATCHES)
		c->break_patches[d][c->break_counts[d]++] = patch_ip;
}

static void push_continue_scope(Compiler* c) {
	if (c->continue_depth < MAX_BREAK_DEPTH) {
		c->continue_counts[c->continue_depth] = 0;
		c->continue_depth++;
	}
}

static void pop_continue_scope(Compiler* c, int target) {
	if (c->continue_depth == 0) return;
	c->continue_depth--;
	int d = c->continue_depth;
	for (int i = 0; i < c->continue_counts[d]; i++) {
		int p = c->continue_patches[d][i];
		c->chunk->code[p].a = target - p - 1;
	}
}

static void emit_continue_jump(Compiler* c, int line, int col) {
	if (c->continue_depth == 0) {
		fprintf(stderr,
				"%s: continue outside loop\n",
				c->filename ? c->filename : "");
		return;
	}
	int patch_ip = c->chunk->len;
	chunk_emit(c->chunk, OP_JUMP, 0, 0, line, col);
	int d = c->continue_depth - 1;
	if (c->continue_counts[d] < MAX_BREAK_PATCHES)
		c->continue_patches[d][c->continue_counts[d]++] = patch_ip;
}

/* Helper: emit with AST source location */
#define EMIT(op, a, b)                                                         \
	chunk_emit(                                                                \
		c->chunk, (op), (a), (b), (ast ? ast->line : 0), (ast ? ast->col : 0))

static void compile_node(Compiler* c, AST* ast);

/* Compile a sequence, leaving the last value on the stack.
   If count == 0, pushes null. */
static void compile_sequence(Compiler* c, AST** stmts, int count) {
	if (count == 0) {
		chunk_emit(c->chunk, OP_NULL, 0, 0, 0, 0);
		return;
	}
	for (int i = 0; i < count; i++) {
		compile_node(c, stmts[i]);
		if (i < count - 1) chunk_emit(c->chunk, OP_POP, 0, 0, 0, 0);
	}
}

/* Compile a sequence and discard ALL results (for switch case bodies). */
static void compile_sequence_discard(Compiler* c, AST* body) {
	if (!body) return;
	if (body->kind == AST_PROGRAM) {
		for (int i = 0; i < body->u.program.body_count; i++) {
			compile_node(c, body->u.program.body[i]);
			chunk_emit(c->chunk, OP_POP, 0, 0, 0, 0);
		}
	} else {
		compile_node(c, body);
		chunk_emit(c->chunk, OP_POP, 0, 0, 0, 0);
	}
}

/* Compile lambda AST into a new sub-chunk. */
static Chunk* compile_lambda(const Compiler* parent, AST* ast) {
	Chunk* sub = chunk_new();
	sub->source_line = ast->line;
	sub->source_col = ast->col;
	sub->param_count = ast->u.lambda.param_count;
	sub->ret_type = ast->u.lambda.ret_type;

	if (sub->param_count > 0) {
		sub->param_names = malloc((size_t)sub->param_count * sizeof(char*));
		sub->param_types = malloc((size_t)sub->param_count * sizeof(Type*));
		sub->param_defaults = calloc((size_t)sub->param_count, sizeof(Chunk*));
		for (int i = 0; i < sub->param_count; i++) {
			sub->param_names[i] = strdup(ast->u.lambda.param_names[i]);
			sub->param_types[i] = ast->u.lambda.param_types[i];
			if (ast->u.lambda.param_defaults && ast->u.lambda.param_defaults[i]) {
				Chunk* def_ch =
					compile(ast->u.lambda.param_defaults[i], parent->filename);
				/* Propagate array elem type into default chunk (mirrors var decl) */
				Type* pt = ast->u.lambda.param_types
							   ? ast->u.lambda.param_types[i]
							   : NULL;
				if (pt && pt->kind == TYPE_ARRAY && pt->u.array.elem
					&& def_ch->len > 0) {
					def_ch->len--; /* remove OP_HALT */
					int ti = chunk_add_type(def_ch, pt->u.array.elem);
					chunk_emit(def_ch, OP_SET_ARR_ELEM_TYPE, ti, 0, 0, 0);
					chunk_emit(def_ch, OP_HALT, 0, 0, 0, 0);
				}
				sub->param_defaults[i] = def_ch;
			}
		}
	}

	sub->func_type =
		make_func(sub->ret_type, sub->param_types, sub->param_count);

	Compiler sub_c;
	compiler_init(&sub_c, sub, parent->filename);
	compile_sequence(&sub_c, ast->u.lambda.body, ast->u.lambda.body_count);
	chunk_emit(sub, OP_POP, 0, 0, 0, 0);
	chunk_emit(sub, OP_NULL, 0, 0, 0, 0);
	chunk_emit(sub, OP_RETURN, 0, 0, 0, 0);

	return sub;
}

static void compile_node(Compiler* c, AST* ast) {
	if (!ast) {
		chunk_emit(c->chunk, OP_NULL, 0, 0, 0, 0);
		return;
	}

	switch (ast->kind) {

		/* 0: var_decl / const_decl */
		case AST_VAR_DECL: {
			if (ast->u.var_decl.init
				&& ast->u.var_decl.init->kind == AST_LAMBDA) {
				/* Propagate annotated param types into unannotated lambda params */
				Type* vt = ast->u.var_decl.vartype;
				if (vt && vt->kind == TYPE_FUNC) {
					AST* lam = ast->u.var_decl.init;
					int n = lam->u.lambda.param_count < vt->u.func.param_count
								? lam->u.lambda.param_count
								: vt->u.func.param_count;
					for (int i = 0; i < n; i++) {
						if (lam->u.lambda.param_types
							&& lam->u.lambda.param_types[i]
							&& lam->u.lambda.param_types[i]->kind == TYPE_BASIC
							&& lam->u.lambda.param_types[i]->u.basic == BASIC_ANY
							&& vt->u.func.params[i])
							lam->u.lambda.param_types[i] = vt->u.func.params[i];
					}
				}
				Chunk* sub = compile_lambda(c, ast->u.var_decl.init);
				sub->name = strdup(ast->u.var_decl.name);
				int si = chunk_add_sub(c->chunk, sub);
				EMIT(OP_CLOSURE, si, 0);
			} else {
				compile_node(c, ast->u.var_decl.init);
			}
			/* Emit cast if declared type needs it */
			Type* dt = ast->u.var_decl.vartype;
			AST* ini = ast->u.var_decl.init;
			if (dt && dt->kind == TYPE_BASIC && ini && ini->kind == AST_LITERAL
				&& ini->u.literal.type
				&& ini->u.literal.type->kind == TYPE_BASIC) {
				BasicType from = ini->u.literal.type->u.basic;
				BasicType to = dt->u.basic;
				if (from == BASIC_FLOAT && to == BASIC_INT)
					EMIT(OP_TO_INT, 0, 0);
				else if (from == BASIC_INT && to == BASIC_FLOAT)
					EMIT(OP_TO_FLOAT, 0, 0);
			}
			/* Propagate array element type from annotation to runtime value */
			if (dt && dt->kind == TYPE_ARRAY && dt->u.array.elem) {
				int ti = chunk_add_type(c->chunk, dt->u.array.elem);
				EMIT(OP_SET_ARR_ELEM_TYPE, ti, 0);
			}
			/* Check interface type compatibility */
			if (dt && dt->kind == TYPE_INTERFACE) {
				int ti = chunk_add_type(c->chunk, dt);
				EMIT(OP_CHECK_TYPE, ti, 0);
			}
			/* Check function type compatibility */
			if (dt && dt->kind == TYPE_FUNC && ini && ini->kind == AST_LAMBDA) {
				int ti = chunk_add_type(c->chunk, dt);
				EMIT(OP_CHECK_TYPE, ti, 0);
			}
			int ni = chunk_add_name(c->chunk, ast->u.var_decl.name);
			EMIT(ast->u.var_decl.is_const ? OP_DEF_CONST : OP_DEF, ni, 0);
			break;
		}

		/* 1: lambda */
		case AST_LAMBDA: {
			Chunk* sub = compile_lambda(c, ast);
			int si = chunk_add_sub(c->chunk, sub);
			EMIT(OP_CLOSURE, si, 0);
			break;
		}

		/* 2: call */
		case AST_CALL: {
			compile_node(c, ast->u.call.func);
			for (int i = 0; i < ast->u.call.arg_count; i++)
				compile_node(c, ast->u.call.args[i]);
			EMIT(OP_CALL, ast->u.call.arg_count, 0);
			break;
		}

		/* 3: literal */
		case AST_LITERAL: {
			if (!ast->u.literal.type) {
				EMIT(OP_NULL, 0, 0);
				break;
			}
			if (ast->u.literal.type->kind == TYPE_ARRAY) {
				EMIT(OP_MAKE_ARR, 0, 0);
				break;
			}
			if (ast->u.literal.type->kind != TYPE_BASIC) {
				EMIT(OP_NULL, 0, 0);
				break;
			}
			switch (ast->u.literal.type->u.basic) {
				case BASIC_NULL:
					EMIT(OP_NULL, 0, 0);
					break;
				case BASIC_BOOL:
					EMIT(ast->u.literal.val.b ? OP_TRUE : OP_FALSE, 0, 0);
					break;
				case BASIC_INT: {
					int ci = chunk_add_const(c->chunk,
											 make_int(ast->u.literal.val.i));
					EMIT(OP_CONST, ci, 0);
					break;
				}
				case BASIC_FLOAT: {
					int ci = chunk_add_const(c->chunk,
											 make_float(ast->u.literal.val.f));
					EMIT(OP_CONST, ci, 0);
					break;
				}
				case BASIC_STRING: {
					int ci = chunk_add_const(c->chunk,
											 make_string(ast->u.literal.val.s));
					EMIT(OP_CONST, ci, 0);
					break;
				}
				default:
					EMIT(OP_NULL, 0, 0);
					break;
			}
			break;
		}

		/* 4: var_ref */
		case AST_VAR_REF: {
			int ni = chunk_add_name(c->chunk, ast->u.var_ref);
			EMIT(OP_GET, ni, 0);
			break;
		}

		/* 5: string_interp */
		case AST_STRING_INTERP: {
			/* Push each interpolated expression value onto the stack */
			for (int i = 0; i < ast->u.string_interp.expr_count; i++)
				compile_node(c, ast->u.string_interp.exprs[i]);
			InterpEntry e;
			e.tmpl = strdup(ast->u.string_interp.str);
			e.var_count = ast->u.string_interp.expr_count;
			int ii = chunk_add_interp(c->chunk, e);
			EMIT(OP_INTERP, ii, 0);
			break;
		}

		/* 6: program (block) */
		case AST_PROGRAM:
			if (ast->u.program.is_block) {
				EMIT(OP_PUSH_SCOPE, 0, 0);
				compile_sequence(
					c, ast->u.program.body, ast->u.program.body_count);
				EMIT(OP_POP_SCOPE, 0, 0);
			} else {
				compile_sequence(
					c, ast->u.program.body, ast->u.program.body_count);
			}
			break;

		/* 7: interface_decl */
		case AST_INTERFACE_DECL:
			EMIT(OP_NULL, 0, 0);
			break;

		/* 27: enum_decl — compiles to a const object binding */
		case AST_ENUM_DECL: {
			int n = ast->u.enum_decl.member_count;

			EMIT(OP_PUSH_SCOPE, 0, 0);

			/* Define each member in order so later members can reference earlier ones */
			int counter = 0;
			for (int i = 0; i < n; i++) {
				AST* mv = ast->u.enum_decl.member_values[i];
				if (mv) {
					compile_node(c, mv);
					/* Advance compile-time counter if init is a literal int */
					if (mv->kind == AST_LITERAL && mv->u.literal.type
						&& mv->u.literal.type->kind == TYPE_BASIC
						&& mv->u.literal.type->u.basic == BASIC_INT) {
						counter = mv->u.literal.val.i + 1;
					}
				} else {
					int ci = chunk_add_const(c->chunk, make_int(counter));
					EMIT(OP_CONST, ci, 0);
					counter++;
				}
				int ni = chunk_add_name(c->chunk, ast->u.enum_decl.member_names[i]);
				EMIT(OP_DEF_CONST, ni, 0);
				EMIT(OP_POP, 0, 0);
			}

			/* Build object from members (GET each before POP_SCOPE) */
			ObjTemplate tmpl;
			tmpl.count = n;
			tmpl.names = malloc((size_t)n * sizeof(char*));
			for (int i = 0; i < n; i++)
				tmpl.names[i] = strdup(ast->u.enum_decl.member_names[i]);
			int ti = chunk_add_objtmpl(c->chunk, tmpl);

			for (int i = 0; i < n; i++) {
				int ni = chunk_add_name(c->chunk, ast->u.enum_decl.member_names[i]);
				EMIT(OP_GET, ni, 0);
			}
			EMIT(OP_MAKE_OBJ, ti, 0);

			EMIT(OP_POP_SCOPE, 0, 0);

			/* Bind enum name as const in the outer scope */
			int name_i = chunk_add_name(c->chunk, ast->u.enum_decl.name);
			EMIT(OP_DEF_CONST, name_i, 0);
			break;
		}

		/* 8: object_literal */
		case AST_OBJECT_LITERAL: {
			int fc = ast->u.object_literal.field_count;
			ObjTemplate tmpl;
			tmpl.count = fc;
			tmpl.names = malloc((size_t)fc * sizeof(char*));
			for (int i = 0; i < fc; i++)
				tmpl.names[i] = strdup(ast->u.object_literal.names[i]);
			int ti = chunk_add_objtmpl(c->chunk, tmpl);
			for (int i = 0; i < fc; i++) {
				AST* val = ast->u.object_literal.values[i];
				if (val && val->kind == AST_LAMBDA) {
					Chunk* sub = compile_lambda(c, val);
					sub->name = strdup(ast->u.object_literal.names[i]);
					int si = chunk_add_sub(c->chunk, sub);
					EMIT(OP_CLOSURE, si, 0);
				} else {
					compile_node(c, val);
				}
			}
			EMIT(OP_MAKE_OBJ, ti, 0);
			break;
		}

		/* 9: member_access */
		case AST_MEMBER_ACCESS: {
			compile_node(c, ast->u.member_access.object);
			int ni = chunk_add_name(c->chunk, ast->u.member_access.member);
			EMIT(OP_MEMBER, ni, 0);
			break;
		}

		/* 10: return */
		case AST_RETURN:
			compile_node(c, ast->u.return_stmt.expr);
			EMIT(OP_RETURN, 0, 0);
			break;

		/* 11: binary */
		case AST_BINARY: {
			if (ast->u.binary.op == BINARY_AND) {
				compile_node(c, ast->u.binary.left);
				EMIT(OP_DUP, 0, 0);
				int jmpf_ip = c->chunk->len;
				EMIT(OP_JMPF, 0, 0);
				EMIT(OP_POP, 0, 0);
				compile_node(c, ast->u.binary.right);
				c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
				break;
			}
			if (ast->u.binary.op == BINARY_OR) {
				compile_node(c, ast->u.binary.left);
				EMIT(OP_DUP, 0, 0);
				int jmpf_ip = c->chunk->len;
				EMIT(OP_JMPF, 0, 0);
				int jump_ip = c->chunk->len;
				EMIT(OP_JUMP, 0, 0);
				c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
				EMIT(OP_POP, 0, 0);
				compile_node(c, ast->u.binary.right);
				c->chunk->code[jump_ip].a = c->chunk->len - jump_ip - 1;
				break;
			}
			compile_node(c, ast->u.binary.left);
			compile_node(c, ast->u.binary.right);
			switch (ast->u.binary.op) {
				case '+':
					EMIT(OP_ADD, 0, 0);
					break;
				case '-':
					EMIT(OP_SUB, 0, 0);
					break;
				case '*':
					EMIT(OP_MUL, 0, 0);
					break;
				case '/':
					EMIT(OP_DIV, 0, 0);
					break;
				case '%':
					EMIT(OP_MOD, 0, 0);
					break;
				case '<':
					EMIT(OP_LT, 0, 0);
					break;
				case '>':
					EMIT(OP_GT, 0, 0);
					break;
				case '&':
					EMIT(OP_BIT_AND, 0, 0);
					break;
				case '|':
					EMIT(OP_BIT_OR, 0, 0);
					break;
				case '^':
					EMIT(OP_BIT_XOR, 0, 0);
					break;
				case BINARY_EQ:
					EMIT(OP_EQ, 0, 0);
					break;
				case BINARY_NEQ:
					EMIT(OP_NEQ, 0, 0);
					break;
				case BINARY_LE:
					EMIT(OP_LE, 0, 0);
					break;
				case BINARY_GE:
					EMIT(OP_GE, 0, 0);
					break;
				case BINARY_SHL:
					EMIT(OP_SHL, 0, 0);
					break;
				case BINARY_SHR:
					EMIT(OP_SHR, 0, 0);
					break;
				default:
					EMIT(OP_NULL, 0, 0);
					break;
			}
			break;
		}

		/* 12: import_decl, 13: export_decl — handled by main.c */
		case AST_IMPORT_DECL:
		case AST_EXPORT_DECL:
			EMIT(OP_NULL, 0, 0);
			break;

		/* 14: if_stmt */
		case AST_IF: {
			compile_node(c, ast->u.if_stmt.cond);
			int jmpf_ip = c->chunk->len;
			EMIT(OP_JMPF, 0, 0);

			compile_node(c, ast->u.if_stmt.then_branch);

			if (ast->u.if_stmt.else_branch) {
				int jump_ip = c->chunk->len;
				EMIT(OP_JUMP, 0, 0);
				c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
				compile_node(c, ast->u.if_stmt.else_branch);
				c->chunk->code[jump_ip].a = c->chunk->len - jump_ip - 1;
			} else {
				/* No else: then-branch left a value; jump over a null push. */
				int jump_ip = c->chunk->len;
				EMIT(OP_JUMP, 0, 0);
				c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
				EMIT(OP_NULL, 0, 0);
				c->chunk->code[jump_ip].a = c->chunk->len - jump_ip - 1;
			}
			break;
		}

		/* 15: while_stmt */
		case AST_WHILE: {
			int loop_start = c->chunk->len;
			compile_node(c, ast->u.while_stmt.cond);
			int jmpf_ip = c->chunk->len;
			EMIT(OP_JMPF, 0, 0);

			push_break_scope(c);
			push_continue_scope(c);
			EMIT(OP_PUSH_SCOPE, 0, 0);
			compile_node(c, ast->u.while_stmt.body);
			EMIT(OP_POP_SCOPE, 0, 0);
			EMIT(OP_POP, 0, 0);

			pop_continue_scope(c, c->chunk->len);
			int loop_ip = c->chunk->len;
			EMIT(OP_LOOP, loop_ip + 1 - loop_start, 0);
			c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
			pop_break_scope(c, c->chunk->len);
			EMIT(OP_NULL, 0, 0);
			break;
		}

		/* 16: do_while_stmt */
		case AST_DO_WHILE: {
			int loop_start = c->chunk->len;
			push_break_scope(c);
			push_continue_scope(c);
			EMIT(OP_PUSH_SCOPE, 0, 0);
			compile_node(c, ast->u.do_while_stmt.body);
			EMIT(OP_POP_SCOPE, 0, 0);
			EMIT(OP_POP, 0, 0);
			pop_continue_scope(c, c->chunk->len);
			compile_node(c, ast->u.do_while_stmt.cond);
			int jmpf_ip = c->chunk->len;
			EMIT(OP_JMPF, 0, 0);
			int loop_ip = c->chunk->len;
			EMIT(OP_LOOP, loop_ip + 1 - loop_start, 0);
			c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
			pop_break_scope(c, c->chunk->len);
			EMIT(OP_NULL, 0, 0);
			break;
		}

		/* 17: for_stmt */
		case AST_FOR: {
			/* outer scope: keeps init variable (e.g. let i) out of the caller
			 */
			EMIT(OP_PUSH_SCOPE, 0, 0);
			if (ast->u.for_stmt.init) {
				compile_node(c, ast->u.for_stmt.init);
				EMIT(OP_POP, 0, 0);
			}
			int loop_start = c->chunk->len;
			int jmpf_ip = -1;
			if (ast->u.for_stmt.cond) {
				compile_node(c, ast->u.for_stmt.cond);
				jmpf_ip = c->chunk->len;
				EMIT(OP_JMPF, 0, 0);
			}
			push_break_scope(c);
			push_continue_scope(c);
			EMIT(OP_PUSH_SCOPE, 0, 0);
			compile_node(c, ast->u.for_stmt.body);
			EMIT(OP_POP_SCOPE, 0, 0);
			EMIT(OP_POP, 0, 0);
			pop_continue_scope(c, c->chunk->len);
			if (ast->u.for_stmt.update) {
				compile_node(c, ast->u.for_stmt.update);
				EMIT(OP_POP, 0, 0);
			}
			int loop_ip = c->chunk->len;
			EMIT(OP_LOOP, loop_ip + 1 - loop_start, 0);
			if (jmpf_ip >= 0)
				c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
			pop_break_scope(c, c->chunk->len);
			EMIT(OP_POP_SCOPE, 0, 0); /* pop outer scope (removes init var) */
			EMIT(OP_NULL, 0, 0);
			break;
		}

		/* 18: switch_stmt
		   Case bodies are compiled with discard so break always jumps at
		   depth=0. Switch result is always null (matches tree-walker for
		   side-effect use). */
		case AST_SWITCH: {
			compile_node(c, ast->u.switch_stmt.expr);
			push_break_scope(c);

			int nc = ast->u.switch_stmt.case_count;
			int* jmpf_patches = malloc((size_t)nc * sizeof(int));
			int* jump_to_end = malloc((size_t)(nc + 1) * sizeof(int));
			int njump = 0;

			for (int i = 0; i < nc; i++) {
				EMIT(OP_DUP, 0, 0);
				compile_node(c, ast->u.switch_stmt.case_values[i]);
				EMIT(OP_EQ, 0, 0);
				jmpf_patches[i] = c->chunk->len;
				EMIT(OP_JMPF, 0, 0);
				/* Match: pop switch expr, run body (discarded), jump to end. */
				EMIT(OP_POP, 0, 0);
				compile_sequence_discard(c, ast->u.switch_stmt.case_bodies[i]);
				jump_to_end[njump++] = c->chunk->len;
				EMIT(OP_JUMP, 0, 0);
				c->chunk->code[jmpf_patches[i]].a =
					c->chunk->len - jmpf_patches[i] - 1;
			}

			/* No match: discard switch expr, run default or skip. */
			EMIT(OP_POP, 0, 0);
			if (ast->u.switch_stmt.default_body)
				compile_sequence_discard(c, ast->u.switch_stmt.default_body);

			int switch_end = c->chunk->len;
			for (int i = 0; i < njump; i++)
				c->chunk->code[jump_to_end[i]].a =
					switch_end - jump_to_end[i] - 1;
			pop_break_scope(c, switch_end);
			free(jmpf_patches);
			free(jump_to_end);
			EMIT(OP_NULL, 0, 0);
			break;
		}

		/* 19: break_stmt */
		case AST_BREAK:
			emit_break_jump(c, ast->line, ast->col);
			EMIT(OP_NULL, 0, 0);
			break;

		/* 26: continue_stmt */
		case AST_CONTINUE:
			emit_continue_jump(c, ast->line, ast->col);
			EMIT(OP_NULL, 0, 0);
			break;

		/* 20: assign */
		case AST_ASSIGN: {
			compile_node(c, ast->u.assign.value);
			int ni = chunk_add_name(c->chunk, ast->u.assign.name);
			EMIT(OP_SET, ni, 0);
			break;
		}

		/* 21: unary */
		case AST_UNARY: {
			int op = ast->u.unary.op;
			if (op == UNARY_PRE_INC || op == UNARY_PRE_DEC
				|| op == UNARY_POST_INC || op == UNARY_POST_DEC) {
				AST* operand = ast->u.unary.operand;
				if (operand->kind == AST_VAR_REF) {
					int ni = chunk_add_name(c->chunk, operand->u.var_ref);
					compile_node(c, operand);
					int one = chunk_add_const(c->chunk, make_int(1));
					EMIT(OP_CONST, one, 0);
					if (op == UNARY_PRE_INC || op == UNARY_POST_INC)
						EMIT(OP_ADD, 0, 0);
					else
						EMIT(OP_SUB, 0, 0);
					EMIT(OP_SET, ni, 0);
					break;
				}
			}
			compile_node(c, ast->u.unary.operand);
			if (op == '-') {
				EMIT(OP_NEG, 0, 0);
			} else if (op == '!') {
				EMIT(OP_NOT, 0, 0);
			} else if (op == UNARY_BIT_NOT) {
				EMIT(OP_BIT_NOT, 0, 0);
			} else {
				/* unary plus */
			}
			break;
		}

		/* 22: conditional */
		case AST_CONDITIONAL: {
			compile_node(c, ast->u.conditional.cond);
			int jmpf_ip = c->chunk->len;
			EMIT(OP_JMPF, 0, 0);
			compile_node(c, ast->u.conditional.true_branch);
			int jump_ip = c->chunk->len;
			EMIT(OP_JUMP, 0, 0);
			c->chunk->code[jmpf_ip].a = c->chunk->len - jmpf_ip - 1;
			compile_node(c, ast->u.conditional.false_branch);
			c->chunk->code[jump_ip].a = c->chunk->len - jump_ip - 1;
			break;
		}

		/* 23: array_literal */
		case AST_ARRAY_LITERAL: {
			int n = ast->u.array_literal.count;
			for (int i = 0; i < n; i++)
				compile_node(c, ast->u.array_literal.values[i]);
			EMIT(OP_MAKE_ARR, n, 0);
			break;
		}

		/* 24: index_access */
		case AST_INDEX_ACCESS: {
			compile_node(c, ast->u.index_access.object);
			compile_node(c, ast->u.index_access.index);
			EMIT(OP_INDEX, 0, 0);
			break;
		}

		/* 25: index_assign  arr[i] = value */
		case AST_INDEX_ASSIGN: {
			compile_node(c, ast->u.index_assign.object);
			compile_node(c, ast->u.index_assign.index);
			compile_node(c, ast->u.index_assign.value);
			EMIT(OP_INDEX_SET, 0, 0);
			break;
		}

		case AST_MEMBER_ASSIGN: {
			compile_node(c, ast->u.member_assign.object);
			compile_node(c, ast->u.member_assign.value);
			int ni = chunk_add_name(c->chunk, ast->u.member_assign.member);
			EMIT(OP_MEMBER_SET, ni, 0);
			break;
		}

		default:
			EMIT(OP_NULL, 0, 0);
			break;
	}
}

/* ── Public entry point ──────────────────────────────────────── */

Chunk* compile(AST* ast, const char* filename) {
	Chunk* ch = chunk_new();
	Compiler c;
	compiler_init(&c, ch, filename);
	compile_node(&c, ast);
	chunk_emit(ch, OP_HALT, 0, 0, 0, 0);
	return ch;
}
