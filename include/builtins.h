#ifndef BUILTINS_H
#define BUILTINS_H

#include "types.h"

typedef Value* (*VMBuiltinFn)(Value** args, int n);

typedef struct {
	const char* sentinel;
	VMBuiltinFn fn;
} BuiltinEntry;

typedef struct {
	const char* mod;
	const char* mem;
	const char* sentinel;
} MemberEntry;

extern const BuiltinEntry BUILTINS[];
extern const MemberEntry MEMBERS[];

VMBuiltinFn builtin_lookup(const char* sentinel);

void builtin_set_error(const char* fmt, ...);
const char* builtin_get_error(void);
void builtin_clear_error(void);

Value* builtin_console_write(Value** args, int n);
Value* builtin_console_writeln(Value** args, int n);
Value* builtin_console_read(Value** args, int n);
Value* builtin_console_readln(Value** args, int n);
Value* builtin_system_quit(Value** args, int n);
Value* builtin_system_args(Value** args, int n);

Value* builtin_array_push(Value** args, int n);
Value* builtin_array_pop(Value** args, int n);
Value* builtin_array_length(Value** args, int n);
Value* builtin_array_get(Value** args, int n);
Value* builtin_array_set(Value** args, int n);
Value* builtin_array_sort(Value** args, int n);
Value* builtin_array_copy(Value** args, int n);

Value* builtin_struct_set(Value** args, int n);
Value* builtin_struct_get(Value** args, int n);
Value* builtin_struct_delete(Value** args, int n);
Value* builtin_struct_clear(Value** args, int n);
Value* builtin_struct_get_keys(Value** args, int n);
Value* builtin_struct_get_values(Value** args, int n);

Value* builtin_json_stringify(Value** args, int n);
Value* builtin_json_parse(Value** args, int n);

Value* builtin_cast_to_bool(Value** args, int n);
Value* builtin_cast_to_string(Value** args, int n);
Value* builtin_cast_to_int(Value** args, int n);
Value* builtin_cast_to_float(Value** args, int n);

Value* builtin_string_length(Value** args, int n);
Value* builtin_string_lowercase(Value** args, int n);
Value* builtin_string_uppercase(Value** args, int n);
Value* builtin_string_trim(Value** args, int n);
Value* builtin_string_split(Value** args, int n);
Value* builtin_string_is_bool(Value** args, int n);
Value* builtin_string_is_int(Value** args, int n);
Value* builtin_string_is_float(Value** args, int n);

Value* builtin_type_of(Value** args, int n);

Value* builtin_file_exist(Value** args, int n);
Value* builtin_file_create(Value** args, int n);
Value* builtin_file_delete(Value** args, int n);
Value* builtin_file_open(Value** args, int n);
Value* builtin_file_close(Value** args, int n);
Value* builtin_file_move(Value** args, int n);
Value* builtin_file_copy(Value** args, int n);
Value* builtin_file_read(Value** args, int n);
Value* builtin_file_write(Value** args, int n);
Value* builtin_file_append(Value** args, int n);
Value* builtin_file_read_bytes(Value** args, int n);
Value* builtin_file_write_bytes(Value** args, int n);
Value* builtin_file_eof(Value** args, int n);
Value* builtin_file_size(Value** args, int n);
Value* builtin_file_mkdir(Value** args, int n);

Value* builtin_time_now(Value** args, int n);
Value* builtin_time_sleep(Value** args, int n);

Value* make_builtin_closure(VMBuiltinFn fn, Type* type);
Env* make_builtin_exports(void);

#endif
