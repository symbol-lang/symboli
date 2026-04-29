#ifndef VM_H
#define VM_H

#include <stdint.h>
#include "types.h"
#include "ast.h"
#include "builtins.h"

/* ── Opcodes ─────────────────────────────────────────────────── */
typedef enum {
    OP_NULL,    /* push null                                        */
    OP_TRUE,    /* push true                                        */
    OP_FALSE,   /* push false                                       */
    OP_CONST,   /* push consts[a]                                   */

    OP_GET,     /* push env_get(env, names[a])                      */
    OP_SET,     /* env_set(env, names[a], peek())  — no pop         */
    OP_DEF,     /* env = env_add(env, names[a], peek()) — no pop    */

    OP_POP,     /* discard top                                      */
    OP_DUP,     /* duplicate top                                    */

    OP_ADD, OP_SUB,
    OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_NOT, OP_NEG,
    OP_BIT_AND, OP_BIT_OR, OP_BIT_XOR, OP_BIT_NOT,
    OP_SHL, OP_SHR,

    OP_JUMP,    /* ip += a  (forward skip)                          */
    OP_JMPF,    /* if !truthy(pop): ip += a                         */
    OP_LOOP,    /* ip -= a  (backward)                              */

    OP_CLOSURE, /* push Closure(subs[a], current env)               */
    OP_CALL,    /* a = arg count                                    */
    OP_RETURN,

    OP_MEMBER,      /* pop obj, push obj.names[a]                   */
    OP_MEMBER_SET,  /* pop value, pop obj; obj.names[a]=value; push value */
    OP_MAKE_OBJ,/* a = obj_templates index; pops field values       */

    OP_MAKE_ARR,/* a = element count; pops N values, builds array   */
    OP_MAKE_TYPED_ARR,/* a = type index; b = element count; pops N values, builds typed array */
    OP_INDEX,   /* pop index, pop array, push array[index]          */
    OP_INDEX_SET,/* pop value, index, array; array[i]=v; push value */
    OP_SET_ARR_ELEM_TYPE, /* a = type_pool index; peek TOS array, set its elem type */
    OP_CHECK_TYPE,        /* a = type_pool index; peek TOS, assert type_is_assignable */

    OP_DEF_CONST,/* like OP_DEF but marks entry as const            */
    OP_TO_INT,  /* pop value, push integer cast                     */
    OP_TO_FLOAT,/* pop value, push float cast                       */

    OP_INTERP,  /* a = interp entry index; push interpolated string */

    OP_PUSH_SCOPE, /* vm->env = env_push_scope(vm->env)           */
    OP_POP_SCOPE,  /* pop env back past the scope sentinel         */

    OP_HALT,
} OpCode;

/* ── Single instruction ──────────────────────────────────────── */
typedef struct {
    uint8_t op;
    int32_t a;
    int32_t b;
} Instr;

/* ── String-interp template ──────────────────────────────────── */
typedef struct {
    char* tmpl;
    int   var_count;
} InterpEntry;

/* ── Object-literal template ─────────────────────────────────── */
typedef struct {
    char** names;
    int    count;
} ObjTemplate;

/* ── Compiled chunk (function or top-level program) ─────────── */
typedef struct Chunk {
    Instr*        code;     int len,     cap;
    Value**       consts;   int nconst,  constcap;
    char**        names;    int nname,   namecap;
    struct Chunk** subs;    int nsub,    subcap;
    InterpEntry*  interps;  int ninterp, interpcap;
    ObjTemplate*  objtmpls; int nobjt,   objtcap;
    Type**        type_pool; int ntype,  typecap; /* runtime type constants */
    int*          lines;    /* parallel to code[] */
    int*          cols;

    int           source_line;
    int           source_col;
    char*         name; /* function name, NULL for anonymous lambdas */

    /* Lambda metadata (set when this chunk IS a function) */
    char**         param_names;
    Type**         param_types;
    struct Chunk** param_defaults; /* NULL entry = no default for that param */
    int            param_count;
    int            is_variadic; /* 1 if last param collects extra args as array */
    Type*          ret_type;
    Type*          func_type; /* cached make_func result */
} Chunk;

/* ── Closure value ───────────────────────────────────────────── */
typedef struct Closure {
    Chunk* chunk;
    Env*   env;
    int    is_builtin;
    VMBuiltinFn builtin_fn;
} Closure;

/* ── Call frame ──────────────────────────────────────────────── */
typedef struct {
    Chunk* chunk;
    int    ip;
    Env*   saved_env; /* env restored on OP_RETURN */
} CallFrame;

/* ── Virtual machine ─────────────────────────────────────────── */
#define VM_STACK_MAX  1024
#define VM_FRAMES_MAX  256

typedef struct {
    Value*     stack[VM_STACK_MAX];
    int        stack_top;
    CallFrame  frames[VM_FRAMES_MAX];
    int        frame_count;
    Env*       env;
    const char* filename;
    Chunk**    owned_chunks;
    int        owned_count;
    int        owned_cap;
} VM;

/* ── Public API ──────────────────────────────────────────────── */
Chunk*  chunk_new(void);
void    chunk_free(Chunk* ch);
Chunk*  compile(AST* ast, const char* filename);

VM*     vm_new(void);
void    vm_free(VM* vm);
Value*  vm_run(VM* vm, Chunk* chunk, Env** env);

VM*     vm_get_global(void);
Value*  vm_call_value(VM* vm, Value* fn, Value** args, int n);

void    chunk_disassemble(const Chunk* ch, const char* label, int depth);

#endif
