#include "vm.h"
#include "builtins.h"
#include "eval.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── VM helpers ─────────────────────────────────────────────── */

VM* vm_new(void) {
	VM* vm = calloc(1, sizeof(VM));
	return vm;
}

void vm_free(VM* vm) {
	for (int i = 0; i < vm->owned_count; i++)
		chunk_free(vm->owned_chunks[i]);
	free(vm->owned_chunks);
	free(vm);
}

static void vm_own_chunk(VM* vm, Chunk* ch) {
	if (vm->owned_count == vm->owned_cap) {
		vm->owned_cap = vm->owned_cap ? vm->owned_cap * 2 : 8;
		vm->owned_chunks =
			realloc(vm->owned_chunks, (size_t)vm->owned_cap * sizeof(Chunk*));
	}
	vm->owned_chunks[vm->owned_count++] = ch;
}

static void vm_push(VM* vm, Value* v) {
	if (vm->stack_top >= VM_STACK_MAX) {
		fprintf(stderr, "Runtime error: stack overflow\n");
		return;
	}
	vm->stack[vm->stack_top++] = v;
}

static Value* vm_pop(VM* vm) {
	if (vm->stack_top == 0) return make_null();
	return vm->stack[--vm->stack_top];
}

static Value* vm_peek(VM* vm) {
	if (vm->stack_top == 0) return make_null();
	return vm->stack[vm->stack_top - 1];
}

/* ── Error reporting ─────────────────────────────────────────── */

static void vm_error(VM* vm, Chunk* ch, int ip, const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	int line = (ch && ip > 0 && ip - 1 < ch->len) ? ch->lines[ip - 1] : 0;
	int col = (ch && ip > 0 && ip - 1 < ch->len) ? ch->cols[ip - 1] : 0;
	if (vm->filename && line) {
		if (col)
			fprintf(stderr, "%s:%d:%d: %s\n", vm->filename, line, col, buf);
		else
			fprintf(stderr, "%s:%d: %s\n", vm->filename, line, buf);
	} else {
		fprintf(stderr, "%s\n", buf);
	}

	if (!vm || vm->frame_count == 0) return;

	fprintf(stderr, "Call stack (most recent call first):\n");

#define STACK_EDGE 5
	int total = vm->frame_count;
	int skipped = total > STACK_EDGE * 2 ? total - STACK_EDGE * 2 : 0;

	for (int i = vm->frame_count - 1; i >= 0; i--) {
		int pos = vm->frame_count - 1 - i; /* 0 = most recent */

		if (skipped) {
			if (pos == STACK_EDGE) {
				fprintf(stderr,
						"  ... %d more frame%s ...\n",
						skipped,
						skipped == 1 ? "" : "s");
			}
			if (pos >= STACK_EDGE && pos < total - STACK_EDGE) continue;
		}

		CallFrame* f = &vm->frames[i];
		Chunk* fc = f->chunk;
		/* top frame: use the live ip passed in; caller frames: ip points past
		 * OP_CALL */
		int fip = (i == vm->frame_count - 1) ? ip : f->ip;
		int fline =
			(fc && fip > 0 && fip - 1 < fc->len) ? fc->lines[fip - 1] : 0;
		int fcol = (fc && fip > 0 && fip - 1 < fc->len) ? fc->cols[fip - 1] : 0;
		const char* fname =
			(fc && fc->name) ? fc->name : (i == 0 ? "<module>" : "<anonymous>");
		if (vm->filename && fline) {
			if (fcol)
				fprintf(stderr,
						"  at %s (%s:%d:%d)\n",
						fname,
						vm->filename,
						fline,
						fcol);
			else
				fprintf(
					stderr, "  at %s (%s:%d)\n", fname, vm->filename, fline);
		} else {
			fprintf(stderr, "  at %s\n", fname);
		}
	}
#undef STACK_EDGE
}

/* ── Value helpers ───────────────────────────────────────────── */

static int is_truthy(Value* val) {
	if (!val) return 0;
	if (val->type->kind != TYPE_BASIC) return 1;
	switch (val->type->u.basic) {
		case BASIC_NULL:
			return 0;
		case BASIC_BOOL:
			return val->u.b;
		case BASIC_INT:
			return val->u.i != 0;
		case BASIC_FLOAT:
			return val->u.f != 0.0;
		case BASIC_STRING:
			return val->u.s && val->u.s[0] != '\0';
		default:
			return 1;
	}
}

static int values_equal(Value* a, Value* b) {
	if (!a || !b) return 0;
	if (a->type->kind != TYPE_BASIC || b->type->kind != TYPE_BASIC) return 0;
	if (a->type->u.basic != b->type->u.basic) {
		int ai = a->type->u.basic, bi = b->type->u.basic;
		if ((ai == BASIC_INT || ai == BASIC_FLOAT)
			&& (bi == BASIC_INT || bi == BASIC_FLOAT)) {
			double av = ai == BASIC_FLOAT ? a->u.f : (double)a->u.i;
			double bv = bi == BASIC_FLOAT ? b->u.f : (double)b->u.i;
			return av == bv;
		}
		return 0;
	}
	switch (a->type->u.basic) {
		case BASIC_NULL:
			return 1;
		case BASIC_BOOL:
			return a->u.b == b->u.b;
		case BASIC_INT:
			return a->u.i == b->u.i;
		case BASIC_FLOAT:
			return a->u.f == b->u.f;
		case BASIC_STRING:
			return strcmp(a->u.s, b->u.s) == 0;
		default:
			return 0;
	}
}

/* ── String interpolation ─────────────────────────────────────── */

static Value* vm_interp(InterpEntry* e, Value** vals) {
	char* result = strdup(e->tmpl);
	for (int i = 0; i < e->var_count; i++) {
		char* val_str = value_to_string(vals[i]);
		char placeholder[32];
		snprintf(placeholder, sizeof(placeholder), "${%d}", i);
		char* found = strstr(result, placeholder);
		if (found) {
			size_t plen = strlen(placeholder);
			size_t before = (size_t)(found - result);
			size_t after = strlen(found + plen);
			char* newstr = malloc(before + strlen(val_str) + after + 1);
			memcpy(newstr, result, before);
			newstr[before] = '\0';
			strcat(newstr, val_str);
			strcat(newstr, found + plen);
			free(result);
			result = newstr;
		}
		free(val_str);
	}
	Value* v = make_string(result);
	free(result);
	return v;
}

/* ── Main execution loop ─────────────────────────────────────── */

Value* vm_run(VM* vm, Chunk* chunk, Env** env) {
	vm->env = *env;
	vm->stack_top = 0;
	vm->frame_count = 0;

	vm->frames[vm->frame_count++] = (CallFrame){ chunk, 0, NULL };

#define FRAME (&vm->frames[vm->frame_count - 1])
#define CH	  (FRAME->chunk)
#define IP	  (FRAME->ip)

	for (;;) {
		if (vm->frame_count == 0) break;
		if (IP >= CH->len) break;

		Chunk* ch = CH;
		Instr inst = ch->code[IP++];

		switch ((OpCode)inst.op) {

			case OP_NULL:
				vm_push(vm, make_null());
				break;
			case OP_TRUE:
				vm_push(vm, make_bool(1));
				break;
			case OP_FALSE:
				vm_push(vm, make_bool(0));
				break;
			case OP_CONST:
				vm_push(vm, ch->consts[inst.a]);
				break;

			case OP_GET: {
				Value* v = env_get(vm->env, ch->names[inst.a]);
				if (!v) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: undefined variable '%s'",
							 ch->names[inst.a]);
					goto vm_error;
				}
				vm_push(vm, v);
				break;
			}

			case OP_SET: {
				Value* v = vm_peek(vm);
				int r = env_set(vm->env, ch->names[inst.a], v);
				if (r == -1) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: assignment to constant '%s'",
							 ch->names[inst.a]);
					goto vm_error;
				}
				if (r == 0) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: undefined variable '%s'",
							 ch->names[inst.a]);
					goto vm_error;
				}
				break;
			}

			case OP_DEF: {
				Value* v = vm_peek(vm);
				if (env_has_local(vm->env, ch->names[inst.a])) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: variable '%s' already declared",
							 ch->names[inst.a]);
					goto vm_error;
				}
				vm->env = env_add(vm->env, ch->names[inst.a], v);
				break;
			}

			case OP_DEF_CONST: {
				Value* v = vm_peek(vm);
				if (env_has_local(vm->env, ch->names[inst.a])) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: '%s' already declared",
							 ch->names[inst.a]);
					goto vm_error;
				}
				vm->env = env_add_const(vm->env, ch->names[inst.a], v);
				break;
			}

			case OP_POP:
				vm_pop(vm);
				break;

			case OP_DUP:
				vm_push(vm, vm_peek(vm));
				break;

			/* ── Arithmetic / comparison ── */
			case OP_ADD: {
				Value* r = vm_pop(vm);
				Value* l = vm_pop(vm);
				if (!l || !r) goto vm_error;
				/* String concatenation */
				if (l->type->kind == TYPE_BASIC
					&& l->type->u.basic == BASIC_STRING
					&& r->type->kind == TYPE_BASIC
					&& r->type->u.basic == BASIC_STRING) {
					size_t len = strlen(l->u.s ? l->u.s : "")
								 + strlen(r->u.s ? r->u.s : "") + 1;
					char* s = malloc(len);
					strcpy(s, l->u.s ? l->u.s : "");
					strcat(s, r->u.s ? r->u.s : "");
					Value* v = make_string(s);
					free(s);
					vm_push(vm, v);
					break;
				}
				if (l->type->kind != TYPE_BASIC
					|| r->type->kind != TYPE_BASIC
					|| l->type->u.basic == BASIC_STRING
					|| r->type->u.basic == BASIC_STRING) {
					vm_error(
						vm, ch, IP, "Type error: '+' expects numeric operands");
					goto vm_error;
				}
				int li = l->u.i, ri = r->u.i;
				int both_int = l->type->u.basic == BASIC_INT
							   && r->type->u.basic == BASIC_INT;
				double lf =
					l->type->u.basic == BASIC_FLOAT ? l->u.f : (double)li;
				double rf =
					r->type->u.basic == BASIC_FLOAT ? r->u.f : (double)ri;
				vm_push(vm, both_int ? make_int(li + ri) : make_float(lf + rf));
				break;
			}

			case OP_SUB: {
				Value* r = vm_pop(vm);
				Value* l = vm_pop(vm);
				if (!l || !r || l->type->kind != TYPE_BASIC
					|| r->type->kind != TYPE_BASIC) {
					vm_error(
						vm, ch, IP, "Type error: '-' expects numeric operands");
					goto vm_error;
				}
				int li = l->u.i, ri = r->u.i;
				int both_int = l->type->u.basic == BASIC_INT
							   && r->type->u.basic == BASIC_INT;
				double lf =
					l->type->u.basic == BASIC_FLOAT ? l->u.f : (double)li;
				double rf =
					r->type->u.basic == BASIC_FLOAT ? r->u.f : (double)ri;
				vm_push(vm, both_int ? make_int(li - ri) : make_float(lf - rf));
				break;
			}

#define ARITH_OP(NAME, INT_EXPR, FLOAT_EXPR)                                   \
	case NAME: {                                                               \
		Value* r = vm_pop(vm);                                                 \
		Value* l = vm_pop(vm);                                                 \
		if (!l || !r || l->type->kind != TYPE_BASIC                            \
			|| r->type->kind != TYPE_BASIC) {                                  \
			vm_error(vm,                                                       \
					 ch,                                                       \
					 IP,                                                       \
					 "Type error: arithmetic operator expects numbers");       \
			goto vm_error;                                                     \
		}                                                                      \
		int li = l->u.i, ri = r->u.i;                                          \
		int both_int =                                                         \
			l->type->u.basic == BASIC_INT && r->type->u.basic == BASIC_INT;    \
		double lf = l->type->u.basic == BASIC_FLOAT ? l->u.f : (double)li;     \
		double rf = r->type->u.basic == BASIC_FLOAT ? r->u.f : (double)ri;     \
		vm_push(vm, both_int ? make_int(INT_EXPR) : make_float(FLOAT_EXPR));   \
		break;                                                                 \
	}
				ARITH_OP(OP_MUL, li * ri, lf * rf)
				ARITH_OP(OP_DIV, li / ri, lf / rf)
				ARITH_OP(OP_MOD, li % ri, fmod(lf, rf))
#undef ARITH_OP

			case OP_NEG: {
				Value* v = vm_pop(vm);
				if (!v || v->type->kind != TYPE_BASIC) {
					vm_error(
						vm, ch, IP, "Type error: unary '-' expects number");
					goto vm_error;
				}
				if (v->type->u.basic == BASIC_FLOAT)
					vm_push(vm, make_float(-v->u.f));
				else
					vm_push(vm, make_int(-v->u.i));
				break;
			}

			case OP_EQ: {
				Value* r = vm_pop(vm);
				Value* l = vm_pop(vm);
				vm_push(vm, make_bool(values_equal(l, r)));
				break;
			}
			case OP_NEQ: {
				Value* r = vm_pop(vm);
				Value* l = vm_pop(vm);
				vm_push(vm, make_bool(!values_equal(l, r)));
				break;
			}

#define CMP_OP(OP_NAME, EXPR)                                                  \
	case OP_NAME: {                                                            \
		Value* r = vm_pop(vm);                                                 \
		Value* l = vm_pop(vm);                                                 \
		if (!l || !r || l->type->kind != TYPE_BASIC                            \
			|| r->type->kind != TYPE_BASIC) {                                  \
			vm_error(vm, ch, IP, "Type error: comparison expects numbers");    \
			goto vm_error;                                                     \
		}                                                                      \
		double lf = l->type->u.basic == BASIC_FLOAT ? l->u.f : (double)l->u.i; \
		double rf = r->type->u.basic == BASIC_FLOAT ? r->u.f : (double)r->u.i; \
		vm_push(vm, make_bool(EXPR));                                          \
		break;                                                                 \
	}

				CMP_OP(OP_LT, lf < rf)
				CMP_OP(OP_GT, lf > rf)
				CMP_OP(OP_LE, lf <= rf)
				CMP_OP(OP_GE, lf >= rf)
#undef CMP_OP

			case OP_NOT: {
				Value* v = vm_pop(vm);
				vm_push(vm, make_bool(!is_truthy(v)));
				break;
			}

#define INT_BINOP(NAME, EXPR)                                                  \
	case NAME: {                                                               \
		Value* r = vm_pop(vm);                                                 \
		Value* l = vm_pop(vm);                                                 \
		if (!l || !r || l->type->kind != TYPE_BASIC                            \
			|| r->type->kind != TYPE_BASIC || l->type->u.basic != BASIC_INT    \
			|| r->type->u.basic != BASIC_INT) {                                \
			vm_error(vm,                                                       \
					 ch,                                                       \
					 IP,                                                       \
					 "Type error: bitwise/shift operator requires integers");  \
			goto vm_error;                                                     \
		}                                                                      \
		vm_push(vm, make_int(EXPR));                                           \
		break;                                                                 \
	}
				INT_BINOP(OP_BIT_AND, l->u.i & r->u.i)
				INT_BINOP(OP_BIT_OR, l->u.i | r->u.i)
				INT_BINOP(OP_BIT_XOR, l->u.i ^ r->u.i)
				INT_BINOP(OP_SHL, l->u.i << r->u.i)
				INT_BINOP(OP_SHR, l->u.i >> r->u.i)
#undef INT_BINOP

			case OP_BIT_NOT: {
				Value* v = vm_pop(vm);
				if (!v || v->type->kind != TYPE_BASIC
					|| v->type->u.basic != BASIC_INT) {
					vm_error(vm, ch, IP, "Type error: '~' requires integer");
					goto vm_error;
				}
				vm_push(vm, make_int(~v->u.i));
				break;
			}

			case OP_TO_INT: {
				Value* v = vm_pop(vm);
				if (!v || v->type->kind != TYPE_BASIC) {
					vm_error(vm, ch, IP, "Type error: cannot cast to integer");
					goto vm_error;
				}
				if (v->type->u.basic == BASIC_FLOAT)
					vm_push(vm, make_int((int)v->u.f));
				else if (v->type->u.basic == BASIC_BOOL)
					vm_push(vm, make_int(v->u.b ? 1 : 0));
				else
					vm_push(vm, v);
				break;
			}

			case OP_TO_FLOAT: {
				Value* v = vm_pop(vm);
				if (!v || v->type->kind != TYPE_BASIC) {
					vm_error(vm, ch, IP, "Type error: cannot cast to float");
					goto vm_error;
				}
				if (v->type->u.basic == BASIC_INT)
					vm_push(vm, make_float((double)v->u.i));
				else
					vm_push(vm, v);
				break;
			}

			/* ── Control flow ── */
			case OP_JUMP:
				IP += inst.a;
				break;

			case OP_JMPF: {
				Value* cond = vm_pop(vm);
				if (!is_truthy(cond)) IP += inst.a;
				break;
			}

			case OP_LOOP:
				IP -= inst.a;
				break;

			/* ── Closure creation ── */
			case OP_CLOSURE: {
				Closure* cl = malloc(sizeof(Closure));
				cl->chunk = ch->subs[inst.a];
				cl->env = vm->env;
				cl->is_builtin = 0;
				cl->builtin_fn = NULL;
				Value* v = malloc(sizeof(Value));
				v->type = cl->chunk->func_type ? cl->chunk->func_type
											   : make_func(NULL, NULL, 0);
				v->u.closure = cl;
				vm_push(vm, v);
				break;
			}

			/* ── Function call ── */
			case OP_CALL: {
				int n = inst.a;
				/* Pop args (stack: [..., func, arg0, arg1, ..., argN-1]) */
				Value** args =
					n > 0 ? malloc((size_t)n * sizeof(Value*)) : NULL;
				for (int i = n - 1; i >= 0; i--)
					args[i] = vm_pop(vm);
				Value* func_val = vm_pop(vm);

				/* Builtin sentinel string? */
				if (func_val->type->kind == TYPE_BASIC
					&& func_val->type->u.basic == BASIC_STRING
					&& func_val->u.s) {
					VMBuiltinFn fn = builtin_lookup(func_val->u.s);
					if (fn) {
						builtin_clear_error();
						Value* res = fn(args, n);
						free(args);
						if (!res) {
							const char* err = builtin_get_error();
							vm_error(vm,
									 ch,
									 IP,
									 "%s",
									 err ? err
										 : "Runtime error: builtin failed");
							goto vm_error;
						}
						vm_push(vm, res);
						break;
					}
				}

				if (func_val->type->kind != TYPE_FUNC) {
					free(args);
					vm_error(
						vm, ch, IP, "Runtime error: value is not callable");
					goto vm_error;
				}

				Closure* cl = func_val->u.closure;

				/* Builtin closure? */
				if (cl->is_builtin) {
					builtin_clear_error();
					Value* res = cl->builtin_fn(args, n);
					free(args);
					if (!res) {
						const char* err = builtin_get_error();
						vm_error(vm,
								 ch,
								 IP,
								 "%s",
								 err ? err : "Runtime error: builtin failed");
						goto vm_error;
					}
					vm_push(vm, res);
					break;
				}

				int provided = n;
				int total = cl->chunk->param_count;

				if (provided > total) {
					free(args);
					char func_loc[64] = "";
					if (cl->chunk->source_line)
						snprintf(func_loc,
								 sizeof(func_loc),
								 " (function defined at %d:%d)",
								 cl->chunk->source_line,
								 cl->chunk->source_col);
					vm_error(vm,
							 ch,
							 IP,
							 "Type error: function%s expected at most %d "
							 "argument(s), got %d",
							 func_loc,
							 total,
							 provided);
					goto vm_error;
				}

				/* Build full argument list (fill defaults if needed) */
				Value** full =
					malloc((size_t)(total > 0 ? total : 1) * sizeof(Value*));
				for (int i = 0; i < provided; i++)
					full[i] = args[i];
				free(args);
				args = NULL;

				for (int i = provided; i < total; i++) {
					if (!cl->chunk->param_defaults
						|| !cl->chunk->param_defaults[i]) {
						vm_error(
							vm,
							ch,
							IP,
							"Type error: missing argument for parameter '%s'",
							cl->chunk->param_names[i]);
						free(full);
						goto vm_error;
					}
					/* Evaluate default in a temporary VM */
					VM* tmp = vm_new();
					tmp->env = vm->env;
					tmp->filename = vm->filename;
					Env* tmp_env = vm->env;
					Value* dv =
						vm_run(tmp, cl->chunk->param_defaults[i], &tmp_env);
					vm_free(tmp);
					if (!dv) {
						free(full);
						goto vm_error;
					}
					full[i] = dv;
				}

				/* Type-check provided args */
				for (int i = 0; i < provided; i++) {
					if (cl->chunk->param_types && cl->chunk->param_types[i]
						&& !type_is_assignable(cl->chunk->param_types[i],
											   full[i]->type)) {
						char* exp = type_to_string(cl->chunk->param_types[i]);
						char* got = type_to_string(full[i]->type);
						char func_loc[64] = "";
						if (cl->chunk->source_line)
							snprintf(func_loc,
									 sizeof(func_loc),
									 " (function defined at %d:%d)",
									 cl->chunk->source_line,
									 cl->chunk->source_col);
						vm_error(
							vm,
							ch,
							IP,
							"Type error: argument %d: expected %s, got %s%s",
							i,
							exp,
							got,
							func_loc);
						free(exp);
						free(got);
						free(full);
						goto vm_error;
					}
				}

				if (vm->frame_count >= VM_FRAMES_MAX) {
					free(full);
					vm_error(vm, ch, IP, "Runtime error: call stack overflow");
					goto vm_error;
				}

				/* Build new env: push scope on top of closure's captured env */
				Env* new_env = env_push_scope(cl->env);
				/* Self-reference: named functions can call themselves */
				if (cl->chunk->name)
					new_env = env_add(new_env, cl->chunk->name, func_val);
				for (int i = 0; i < total; i++)
					new_env =
						env_add(new_env, cl->chunk->param_names[i], full[i]);
				free(full);

				/* Push call frame (saves current execution state) */
				vm->frames[vm->frame_count++] =
					(CallFrame){ cl->chunk, 0, vm->env };
				vm->env = new_env;
				break;
			}

			/* ── Return ── */
			case OP_RETURN: {
				Value* retval = vm_pop(vm);

				/* Type check */
				Type* ret_type = CH->ret_type;
				if (ret_type && !type_is_assignable(ret_type, retval->type)) {
					char* exp = type_to_string(ret_type);
					char* got = type_to_string(retval->type);
					const char* fname = CH->name ? CH->name : "<anonymous>";
					char func_loc[64] = "";
					if (CH->source_line)
						snprintf(func_loc,
								 sizeof(func_loc),
								 " at %d:%d",
								 CH->source_line,
								 CH->source_col);
					vm_error(vm,
							 ch,
							 IP,
							 "Type error: function '%s'%s: expected return "
							 "type %s, got %s",
							 fname,
							 func_loc,
							 exp,
							 got);
					free(exp);
					free(got);
					goto vm_error;
				}

				/* Store inferred/declared return type so type.of() reflects the
			   union without overwriting the concrete type used for runtime ops. */
				if (ret_type) retval->declared_type = ret_type;

				Env* saved = CH->subs ? NULL : NULL; /* silence warning */
				saved = FRAME->saved_env;
				vm->frame_count--;

				if (vm->frame_count == 0) {
					vm->env = saved ? saved : vm->env;
					vm_push(vm, retval);
					goto vm_done;
				}
				vm->env = saved;
				vm_push(vm, retval);
				break;
			}

			/* ── Member access ── */
			case OP_MEMBER_SET: {
				Value* val = vm_pop(vm);
				Value* obj = vm_pop(vm);
				const char* mem = ch->names[inst.a];
				if (!obj || obj->type->kind != TYPE_INTERFACE) {
					vm_error(vm, ch, IP,
							 "Runtime error: member assign on non-object value");
					goto vm_error;
				}
				object_set_field(obj, mem, val);
				vm_push(vm, val);
				break;
			}

			case OP_MEMBER: {
				Value* obj = vm_pop(vm);
				const char* mem = ch->names[inst.a];
				if (!obj) {
					vm_error(vm, ch, IP, "Runtime error: null member access");
					goto vm_error;
				}

				if (!obj->type || obj->type->kind != TYPE_INTERFACE) {
					vm_error(
						vm,
						ch,
						IP,
						"Runtime error: member access on non-object value");
					goto vm_error;
				}
				Value* field = object_get_field(obj, mem);
				if (!field) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: object has no field '%s'",
							 mem);
					goto vm_error;
				}
				vm_push(vm, field);
				break;
			}

			/* ── Object literal ── */
			case OP_MAKE_OBJ: {
				ObjTemplate* tmpl = &ch->objtmpls[inst.a];
				int fc = tmpl->count;
				ObjectField* fields =
					fc > 0 ? malloc((size_t)fc * sizeof(ObjectField)) : NULL;
				InterfaceField* ifields =
					fc > 0 ? malloc((size_t)fc * sizeof(InterfaceField)) : NULL;
				for (int i = fc - 1; i >= 0; i--) {
					Value* v = vm_pop(vm);
					fields[i].name = strdup(tmpl->names[i]);
					fields[i].value = v;
					ifields[i].name = strdup(tmpl->names[i]);
					ifields[i].type = v->type;
				}
				vm_push(
					vm,
					make_object(make_interface(NULL, ifields, fc), fields, fc));
				break;
			}

			/* ── Array literal ── */
			case OP_MAKE_ARR: {
				int n = inst.a;
				/* Determine element type from first element (or any) */
				Type* elem_type = make_basic(BASIC_ANY);
				Value* arr = make_array_value(make_array(elem_type));
				arr->u.arr.data =
					n > 0 ? malloc((size_t)n * sizeof(Value*)) : NULL;
				arr->u.arr.cap = n;
				arr->u.arr.len = n;
				/* Elements were pushed left-to-right; pop in reverse */
				for (int i = n - 1; i >= 0; i--)
					arr->u.arr.data[i] = vm_pop(vm);
				vm_push(vm, arr);
				break;
			}

			/* ── Set typed element type on TOS array ── */
			case OP_SET_ARR_ELEM_TYPE: {
				Value* arr = vm_peek(vm);
				if (arr && arr->type->kind == TYPE_ARRAY) {
					Type* elem_type = ch->type_pool[inst.a];
					arr->type->u.array.elem = elem_type;
					/* Validate existing elements against the declared type */
					for (int i = 0; i < arr->u.arr.len; i++) {
						Value* elem = arr->u.arr.data[i];
						if (elem
							&& !type_is_assignable(elem_type, elem->type)) {
							char* exp = type_to_string(elem_type);
							char* got = type_to_string(elem->type);
							vm_error(vm,
									 ch,
									 IP,
									 "Type error: array element %d: expected "
									 "%s, got %s",
									 i,
									 exp,
									 got);
							free(exp);
							free(got);
							goto vm_error;
						}
					}
				}
				break;
			}

			/* ── Type check (interface or function signature) ── */
			case OP_CHECK_TYPE: {
				Value* v = vm_peek(vm);
				Type* expected = ch->type_pool[inst.a];
				int ok;
				if (expected->kind == TYPE_FUNC && v
					&& v->type->kind == TYPE_FUNC) {
					/* Compare param count and each param type exactly */
					ok = (expected->u.func.param_count
						  == v->type->u.func.param_count);
					for (int pi = 0; ok && pi < expected->u.func.param_count;
						 pi++)
						ok = type_equals(expected->u.func.params[pi],
										 v->type->u.func.params[pi]);
				} else {
					ok = v && type_is_assignable(expected, v->type);
				}
				if (!ok) {
					char* exp = type_to_string(expected);
					if (expected->kind == TYPE_INTERFACE) {
						char* reason =
							type_assignability_error(expected, v->type);
						vm_error(vm, ch, IP,
								 "Type error: value does not satisfy interface "
								 "%s: %s",
								 exp, reason);
						free(reason);
					} else {
						char* got = type_to_string(v->type);
						vm_error(vm, ch, IP,
								 "Type error: expected %s, got %s", exp, got);
						free(got);
					}
					free(exp);
					goto vm_error;
				}
				break;
			}

			/* ── Array/object index access ── */
			case OP_INDEX: {
				Value* idx = vm_pop(vm);
				Value* obj = vm_pop(vm);
				/* String-keyed object (struct) access */
				if (obj && obj->type->kind == TYPE_INTERFACE && idx
					&& idx->type->kind == TYPE_BASIC
					&& idx->type->u.basic == BASIC_STRING) {
					Value* field = object_get_field(obj, idx->u.s);
					vm_push(vm, field ? field : make_null());
					break;
				}
				if (!obj || obj->type->kind != TYPE_ARRAY) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: index access on non-array value");
					goto vm_error;
				}
				if (!idx || idx->type->kind != TYPE_BASIC
					|| idx->type->u.basic != BASIC_INT) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: array index must be an integer");
					goto vm_error;
				}
				int i = idx->u.i;
				if (i < 0 || i >= obj->u.arr.len) {
					vm_error(
						vm,
						ch,
						IP,
						"Runtime error: array index %d out of bounds (len %d)",
						i,
						obj->u.arr.len);
					goto vm_error;
				}
				vm_push(vm, obj->u.arr.data[i]);
				break;
			}

			/* ── Array/object index assign ── */
			case OP_INDEX_SET: {
				Value* val = vm_pop(vm);
				Value* idx = vm_pop(vm);
				Value* obj = vm_pop(vm);
				/* String-keyed object (struct) assign */
				if (obj && obj->type->kind == TYPE_INTERFACE && idx
					&& idx->type->kind == TYPE_BASIC
					&& idx->type->u.basic == BASIC_STRING) {
					object_set_field(obj, idx->u.s, val);
					vm_push(vm, val);
					break;
				}
				if (!obj || obj->type->kind != TYPE_ARRAY) {
					vm_error(vm,
							 ch,
							 IP,
							 "Runtime error: index assign on non-array value");
					goto vm_error;
				}
				int i = idx->u.i;
				if (i < 0 || i >= obj->u.arr.len) {
					vm_error(
						vm,
						ch,
						IP,
						"Runtime error: array index %d out of bounds (len %d)",
						i,
						obj->u.arr.len);
					goto vm_error;
				}
				/* Type-check assignment against declared element type */
				Type* elem_t = obj->type->u.array.elem;
				if (elem_t
					&& !(elem_t->kind == 0 && elem_t->u.basic == BASIC_ANY)
					&& !type_is_assignable(elem_t, val->type)) {
					char* exp = type_to_string(elem_t);
					char* got = type_to_string(val->type);
					vm_error(vm,
							 ch,
							 IP,
							 "Type error: cannot assign %s to %s[] element",
							 got,
							 exp);
					free(exp);
					free(got);
					goto vm_error;
				}
				obj->u.arr.data[i] = val;
				vm_push(vm, val);
				break;
			}

			/* ── String interpolation ── */
			case OP_INTERP: {
				InterpEntry* e = &ch->interps[inst.a];
				Value** vals =
					malloc((size_t)(e->var_count > 0 ? e->var_count : 1)
						   * sizeof(Value*));
				for (int i = e->var_count - 1; i >= 0; i--)
					vals[i] = vm_pop(vm);
				Value* v = vm_interp(e, vals);
				free(vals);
				if (!v) goto vm_error;
				vm_push(vm, v);
				break;
			}

			case OP_PUSH_SCOPE:
				vm->env = env_push_scope(vm->env);
				break;

			case OP_POP_SCOPE:
				while (vm->env && vm->env->name != NULL)
					vm->env = vm->env->next;
				if (vm->env) vm->env = vm->env->next; /* skip sentinel */
				break;

			case OP_HALT:
				goto vm_done;

			default:
				vm_error(
					vm, ch, IP, "Runtime error: unknown opcode %d", inst.op);
				goto vm_error;
		}
	}

vm_done:
	*env = vm->env;
	return vm->stack_top > 0 ? vm->stack[vm->stack_top - 1] : make_null();

vm_error:
	*env = vm->env;
	return NULL;

#undef FRAME
#undef CH
#undef IP
}

/* ── Call-from-C API ─────────────────────────────────────────── */

static VM* g_vm = NULL;

VM* vm_get_global(void) {
	return g_vm;
}

Value* vm_call_value(VM* vm, Value* fn, Value** args, int n) {
	if (!fn || fn->type->kind != TYPE_FUNC) return make_null();
	Closure* cl = fn->u.closure;
	if (cl->is_builtin) {
		builtin_clear_error();
		return cl->builtin_fn(args, n);
	}
	int total = cl->chunk->param_count;
	Env* new_env = env_push_scope(cl->env);
	if (cl->chunk->name) new_env = env_add(new_env, cl->chunk->name, fn);
	int provided = n < total ? n : total;
	for (int i = 0; i < provided; i++)
		new_env = env_add(new_env, cl->chunk->param_names[i], args[i]);
	VM* tmp = vm_new();
	tmp->filename = vm ? vm->filename : NULL;
	Value* result = vm_run(tmp, cl->chunk, &new_env);
	vm_free(tmp);
	return result;
}

/* ── Disassembler ────────────────────────────────────────────── */

static const char* opcode_name(uint8_t op) {
	switch ((OpCode)op) {
		case OP_NULL:
			return "OP_NULL";
		case OP_TRUE:
			return "OP_TRUE";
		case OP_FALSE:
			return "OP_FALSE";
		case OP_CONST:
			return "OP_CONST";
		case OP_GET:
			return "OP_GET";
		case OP_SET:
			return "OP_SET";
		case OP_DEF:
			return "OP_DEF";
		case OP_DEF_CONST:
			return "OP_DEF_CONST";
		case OP_POP:
			return "OP_POP";
		case OP_DUP:
			return "OP_DUP";
		case OP_ADD:
			return "OP_ADD";
		case OP_SUB:
			return "OP_SUB";
		case OP_MUL:
			return "OP_MUL";
		case OP_DIV:
			return "OP_DIV";
		case OP_MOD:
			return "OP_MOD";
		case OP_EQ:
			return "OP_EQ";
		case OP_NEQ:
			return "OP_NEQ";
		case OP_LT:
			return "OP_LT";
		case OP_GT:
			return "OP_GT";
		case OP_LE:
			return "OP_LE";
		case OP_GE:
			return "OP_GE";
		case OP_NOT:
			return "OP_NOT";
		case OP_NEG:
			return "OP_NEG";
		case OP_BIT_AND:
			return "OP_BIT_AND";
		case OP_BIT_OR:
			return "OP_BIT_OR";
		case OP_BIT_XOR:
			return "OP_BIT_XOR";
		case OP_BIT_NOT:
			return "OP_BIT_NOT";
		case OP_SHL:
			return "OP_SHL";
		case OP_SHR:
			return "OP_SHR";
		case OP_JUMP:
			return "OP_JUMP";
		case OP_JMPF:
			return "OP_JMPF";
		case OP_LOOP:
			return "OP_LOOP";
		case OP_CLOSURE:
			return "OP_CLOSURE";
		case OP_CALL:
			return "OP_CALL";
		case OP_RETURN:
			return "OP_RETURN";
		case OP_MEMBER:
			return "OP_MEMBER";
		case OP_MEMBER_SET:
			return "OP_MEMBER_SET";
		case OP_MAKE_OBJ:
			return "OP_MAKE_OBJ";
		case OP_MAKE_ARR:
			return "OP_MAKE_ARR";
		case OP_MAKE_TYPED_ARR:
			return "OP_MAKE_TYPED_ARR";
		case OP_INDEX:
			return "OP_INDEX";
		case OP_INDEX_SET:
			return "OP_INDEX_SET";
		case OP_SET_ARR_ELEM_TYPE:
			return "OP_SET_ARR_ELEM_TYPE";
		case OP_CHECK_TYPE:
			return "OP_CHECK_TYPE";
		case OP_TO_INT:
			return "OP_TO_INT";
		case OP_TO_FLOAT:
			return "OP_TO_FLOAT";
		case OP_INTERP:
			return "OP_INTERP";
		case OP_PUSH_SCOPE:
			return "OP_PUSH_SCOPE";
		case OP_POP_SCOPE:
			return "OP_POP_SCOPE";
		case OP_HALT:
			return "OP_HALT";
		default:
			return "OP_UNKNOWN";
	}
}

void chunk_disassemble(const Chunk* ch, const char* label, int depth) {
	char pad[64];
	int p = depth * 2;
	if (p >= 63) p = 62;
	memset(pad, ' ', (size_t)p);
	pad[p] = '\0';

	printf("%s=== %s ===\n", pad, label ? label : "<anonymous>");

	for (int i = 0; i < ch->len; i++) {
		Instr inst = ch->code[i];
		int line = ch->lines ? ch->lines[i] : 0;
		int col = ch->cols ? ch->cols[i] : 0;

		printf("%s  %04d", pad, i);
		if (line > 0)
			printf("  (%3d:%-3d)", line, col);
		else
			printf("            ");
		printf("  %-22s", opcode_name(inst.op));

		switch ((OpCode)inst.op) {
			case OP_CONST:
				printf(" [%d]", inst.a);
				if (inst.a >= 0 && inst.a < ch->nconst && ch->consts[inst.a]) {
					char* s = value_to_string(ch->consts[inst.a]);
					printf(" = %s", s);
					free(s);
				}
				break;
			case OP_GET:
			case OP_SET:
			case OP_DEF:
			case OP_DEF_CONST:
			case OP_MEMBER:
			case OP_MEMBER_SET:
				if (inst.a >= 0 && inst.a < ch->nname)
					printf(" \"%s\"", ch->names[inst.a]);
				break;
			case OP_CLOSURE:
				printf(" subs[%d]", inst.a);
				if (inst.a >= 0 && inst.a < ch->nsub && ch->subs[inst.a]->name)
					printf(" (\"%s\")", ch->subs[inst.a]->name);
				break;
			case OP_CALL:
				printf(" argc=%d", inst.a);
				break;
			case OP_JUMP:
				printf(" +%d -> %04d", inst.a, i + 1 + inst.a);
				break;
			case OP_JMPF:
				printf(" +%d -> %04d", inst.a, i + 1 + inst.a);
				break;
			case OP_LOOP:
				printf(" -%d -> %04d", inst.a, i + 1 - inst.a);
				break;
			case OP_MAKE_ARR:
				printf(" count=%d", inst.a);
				break;
			case OP_MAKE_TYPED_ARR:
				printf(" type[%d] count=%d", inst.a, inst.b);
				break;
			case OP_MAKE_OBJ:
				printf(" tmpl[%d]", inst.a);
				if (inst.a >= 0 && inst.a < ch->nobjt) {
					ObjTemplate* t = &ch->objtmpls[inst.a];
					printf(" {");
					for (int j = 0; j < t->count; j++) {
						if (j > 0) printf(", ");
						printf("%s", t->names[j]);
					}
					printf("}");
				}
				break;
			case OP_SET_ARR_ELEM_TYPE:
				printf(" type[%d]", inst.a);
				if (inst.a >= 0 && inst.a < ch->ntype
					&& ch->type_pool[inst.a]) {
					char* ts = type_to_string(ch->type_pool[inst.a]);
					printf(" (%s)", ts);
					free(ts);
				}
				break;
			case OP_INTERP:
				printf(" [%d]", inst.a);
				if (inst.a >= 0 && inst.a < ch->ninterp) {
					printf(" \"%s\" (vars=%d)",
						   ch->interps[inst.a].tmpl,
						   ch->interps[inst.a].var_count);
				}
				break;
			default:
				break;
		}
		printf("\n");
	}

	if (ch->nconst > 0) {
		printf("%s  -- constants --\n", pad);
		for (int i = 0; i < ch->nconst; i++) {
			char* s = value_to_string(ch->consts[i]);
			printf("%s  [%d] %s\n", pad, i, s);
			free(s);
		}
	}

	for (int i = 0; i < ch->nsub; i++) {
		Chunk* sub = ch->subs[i];
		char sub_label[256];
		if (sub->name && sub->source_line)
			snprintf(sub_label,
					 sizeof(sub_label),
					 "function \"%s\" (at %d:%d)",
					 sub->name,
					 sub->source_line,
					 sub->source_col);
		else if (sub->name)
			snprintf(
				sub_label, sizeof(sub_label), "function \"%s\"", sub->name);
		else if (sub->source_line)
			snprintf(sub_label,
					 sizeof(sub_label),
					 "anonymous (at %d:%d)",
					 sub->source_line,
					 sub->source_col);
		else
			snprintf(sub_label, sizeof(sub_label), "anonymous");
		printf("\n");
		chunk_disassemble(sub, sub_label, depth + 1);
	}
}

/* ── Public eval API (keeps eval.h interface) ─────────────────── */
static const char* g_filename = NULL;

void eval_set_filename(const char* filename) {
	if (!g_vm) g_vm = vm_new();
	free((char*)g_vm->filename);
	g_vm->filename = filename ? strdup(filename) : NULL;
	g_filename = g_vm->filename;
}

Value* eval(AST* ast, Env** env) {
	if (!g_vm) {
		g_vm = vm_new();
		g_vm->filename = g_filename;
	}
	Chunk* ch = compile(ast, g_vm->filename);
	vm_own_chunk(g_vm, ch);
	return vm_run(g_vm, ch, env);
}
