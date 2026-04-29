#ifndef TYPES_H
#define TYPES_H

#include <stdlib.h>

typedef enum BasicType {
	BASIC_NULL = 0,
	BASIC_BOOL = 1,
	BASIC_INT = 2,
	BASIC_FLOAT = 3,
	BASIC_STRING = 4,
	BASIC_ANY = 5
} BasicType;

typedef struct InterfaceField {
	char* name;
	struct Type* type;
} InterfaceField;

typedef enum TypeKind {
	TYPE_BASIC     = 0,
	TYPE_FUNC      = 1,
	TYPE_ARRAY     = 2,
	TYPE_INTERFACE = 3,
	TYPE_UNION     = 4,
} TypeKind;

typedef struct Type {
	TypeKind kind;
	union {
		BasicType basic;
		struct {
			struct Type* ret;
			struct Type** params;
			int param_count;
		} func;
		struct {
			struct Type* elem;
		} array;
		struct {
			char* name;
			InterfaceField* fields;
			int field_count;
		} iface;
		struct {
			struct Type** types;
			int type_count;
		} union_type;
	} u;
} Type;

struct AST; // forward declaration
struct Closure; // forward declaration (defined in vm.h)

typedef struct ObjectField {
	char* name;
	struct Value* value;
} ObjectField;

typedef struct Value {
	struct Type* type;
	struct Type* declared_type; /* non-NULL when function return type is a union */
	union {
		int b;
		int i;
		double f;
		char* s;
		struct Closure* closure;
		struct {
			ObjectField* fields;
			int field_count;
			int field_cap;
		} object;
		struct {
			struct Value** data;
			int len;
			int cap;
		} arr;
	} u;
} Value;

typedef struct Env {
	char* name;
	struct Value* value;
	struct Env* next;
	int is_const;
} Env;

Type* make_basic(BasicType b);
Type* make_func(Type* ret, Type** params, int pc);
Type* make_array(Type* elem);
Type* make_interface(char* name, InterfaceField* fields, int field_count);
Type* make_union(Type** types, int count);

Type* interface_field_type(const Type* type, const char* field_name);

int type_equals(const Type* left, const Type* right);
int type_is_assignable(const Type* expected, const Type* actual);
char* type_to_string(const Type* type);
char* type_assignability_error(const Type* expected, const Type* actual);

Value* make_null(void);
Value* make_bool(int b);
Value* make_int(int i);
Value* make_float(double f);
Value* make_string(char* s);

Value* make_array_value(Type* t);
void   array_push(Value* arr, Value* elem);

Value* make_object(Type* type, ObjectField* fields, int field_count);
Value* object_get_field(Value* object, const char* field_name);
void   object_set_field(Value* object, const char* name, Value* val);
void   object_delete_field(Value* object, const char* name);
void   object_clear(Value* object);
char* value_to_string(const Value* value);

Env* env_add(Env* env, char* name, Value* val);
Env* env_add_const(Env* env, char* name, Value* val);
Value* env_get(Env* env, char* name);
int env_set(Env* env, char* name, Value* val);
Env* env_push_scope(Env* env);
int env_has_local(Env* env, char* name);

#endif
