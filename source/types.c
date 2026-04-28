#include "types.h"

#include <stdio.h>
#include <string.h>

static char* dup_cstr(const char* s) {
	size_t len = strlen(s);
	char* copy = malloc(len + 1);
	memcpy(copy, s, len + 1);
	return copy;
}

Type* make_basic(BasicType b) {
	Type* t = malloc(sizeof(Type));
	t->kind = TYPE_BASIC;
	t->u.basic = b;
	return t;
}

Type* make_func(Type* ret, Type** params, int pc) {
	Type* t = malloc(sizeof(Type));
	t->kind = TYPE_FUNC;
	t->u.func.ret = ret;
	if (pc > 0) {
		t->u.func.params = calloc((size_t)pc, sizeof(Type*));
		if (params) {
			for (int i = 0; i < pc; i++)
				t->u.func.params[i] = params[i];
		}
	} else {
		t->u.func.params = NULL;
	}
	t->u.func.param_count = pc;
	return t;
}

Type* make_array(Type* elem) {
	Type* t = malloc(sizeof(Type));
	t->kind = TYPE_ARRAY;
	t->u.array.elem = elem;
	return t;
}

Type* make_union(Type** types, int count) {
	Type* t = malloc(sizeof(Type));
	t->kind = TYPE_UNION;
	t->u.union_type.types = malloc(sizeof(Type*) * (size_t)count);
	for (int i = 0; i < count; i++)
		t->u.union_type.types[i] = types[i];
	t->u.union_type.type_count = count;
	return t;
}

Type* make_interface(char* name, InterfaceField* fields, int field_count) {
	Type* t = malloc(sizeof(Type));
	t->kind = TYPE_INTERFACE;
	t->u.iface.name = name ? strdup(name) : NULL;
	t->u.iface.fields = fields;
	t->u.iface.field_count = field_count;
	return t;
}

Type* interface_field_type(const Type* type, const char* field_name) {
	if (!type || type->kind != TYPE_INTERFACE) return NULL;
	for (int i = 0; i < type->u.iface.field_count; i++) {
		if (strcmp(type->u.iface.fields[i].name, field_name) == 0)
			return type->u.iface.fields[i].type;
	}
	return NULL;
}

int type_equals(const Type* left, const Type* right) {
	if (left == right) return 1;
	if (!left || !right) return 0;
	if (left->kind != right->kind) return 0;

	if (left->kind == TYPE_BASIC) return left->u.basic == right->u.basic;

	if (left->kind == TYPE_FUNC) {
		if (!type_equals(left->u.func.ret, right->u.func.ret)) return 0;
		if (left->u.func.param_count != right->u.func.param_count) return 0;
		for (int i = 0; i < left->u.func.param_count; i++) {
			if (!type_equals(left->u.func.params[i], right->u.func.params[i]))
				return 0;
		}
		return 1;
	}

	if (left->kind == TYPE_INTERFACE) {
		if (left->u.iface.name && right->u.iface.name)
			return strcmp(left->u.iface.name, right->u.iface.name) == 0;
		if (left->u.iface.field_count != right->u.iface.field_count) return 0;
		for (int i = 0; i < left->u.iface.field_count; i++) {
			const InterfaceField* lf = &left->u.iface.fields[i];
			const Type* rf_type = interface_field_type(right, lf->name);
			if (!rf_type || !type_equals(lf->type, rf_type)) return 0;
		}
		return 1;
	}

	if (left->kind == TYPE_UNION) {
		if (left->u.union_type.type_count != right->u.union_type.type_count)
			return 0;
		for (int i = 0; i < left->u.union_type.type_count; i++) {
			if (!type_equals(left->u.union_type.types[i],
							 right->u.union_type.types[i]))
				return 0;
		}
		return 1;
	}

	return type_equals(left->u.array.elem, right->u.array.elem);
}

int type_is_assignable(const Type* expected, const Type* actual) {
	if (!expected || !actual) return 0;
	if (type_equals(expected, actual)) return 1;

	if (expected->kind == TYPE_BASIC && expected->u.basic == BASIC_ANY)
		return 1;
	if (actual->kind == TYPE_BASIC && actual->u.basic == BASIC_ANY) return 1;

	if (expected->kind == TYPE_UNION) {
		for (int i = 0; i < expected->u.union_type.type_count; i++) {
			if (type_is_assignable(expected->u.union_type.types[i], actual))
				return 1;
		}
		return 0;
	}
	if (actual->kind == TYPE_UNION) {
		for (int i = 0; i < actual->u.union_type.type_count; i++) {
			if (!type_is_assignable(expected, actual->u.union_type.types[i]))
				return 0;
		}
		return 1;
	}

	if (expected->kind == TYPE_BASIC && actual->kind == TYPE_BASIC) {
		return expected->u.basic == BASIC_FLOAT && actual->u.basic == BASIC_INT;
	}

	if (expected->kind == TYPE_FUNC && actual->kind == TYPE_FUNC) {
		/* Always check parameter count first. */
		if (expected->u.func.param_count != actual->u.func.param_count) return 0;
		for (int i = 0; i < expected->u.func.param_count; i++) {
			if (!type_is_assignable(expected->u.func.params[i],
									actual->u.func.params[i]))
				return 0;
		}
		/* Skip return-type check when either side has no explicit return type. */
		if (!actual->u.func.ret || !expected->u.func.ret) return 1;
		return type_is_assignable(expected->u.func.ret, actual->u.func.ret);
	}

	if (expected->kind == TYPE_ARRAY && actual->kind == TYPE_ARRAY) {
		if (actual->u.array.elem && actual->u.array.elem->kind == TYPE_BASIC
			&& actual->u.array.elem->u.basic == BASIC_NULL)
			return 1;
		return type_is_assignable(expected->u.array.elem, actual->u.array.elem);
	}

	if (expected->kind == TYPE_INTERFACE && actual->kind == TYPE_INTERFACE) {
		for (int i = 0; i < expected->u.iface.field_count; i++) {
			const InterfaceField* field = &expected->u.iface.fields[i];
			const Type* actual_field_type =
				interface_field_type(actual, field->name);
			if (!actual_field_type
				|| !type_is_assignable(field->type, actual_field_type))
				return 0;
		}
		return 1;
	}

	return 0;
}

char* type_to_string(const Type* type) {
	if (!type) return dup_cstr("<unknown>");

	if (type->kind == TYPE_BASIC) {
		switch (type->u.basic) {
			case BASIC_NULL:
				return dup_cstr("null");
			case BASIC_BOOL:
				return dup_cstr("bool");
			case BASIC_INT:
				return dup_cstr("int");
			case BASIC_FLOAT:
				return dup_cstr("float");
			case BASIC_STRING:
				return dup_cstr("string");
			case BASIC_ANY:
				return dup_cstr("any");
		}
	}

	if (type->kind == TYPE_ARRAY) {
		char* elem = type_to_string(type->u.array.elem);
		size_t size = strlen(elem) + 3;
		char* result = malloc(size);
		snprintf(result, size, "%s[]", elem);
		free(elem);
		return result;
	}

	if (type->kind == TYPE_FUNC) {
		char* ret =
			type->u.func.ret ? type_to_string(type->u.func.ret) : dup_cstr("");
		size_t size = strlen(ret) + 3;
		for (int i = 0; i < type->u.func.param_count; i++) {
			char* param = type_to_string(type->u.func.params[i]);
			size += strlen(param) + 2;
			free(param);
		}
		char* result = malloc(size);
		result[0] = '\0';
		strcat(result, ret);
		strcat(result, "(");
		for (int i = 0; i < type->u.func.param_count; i++) {
			char* param = type_to_string(type->u.func.params[i]);
			strcat(result, param);
			if (i + 1 < type->u.func.param_count) strcat(result, ", ");
			free(param);
		}
		strcat(result, ")");
		free(ret);
		return result;
	}

	if (type->kind == TYPE_INTERFACE) {
		if (type->u.iface.name) return dup_cstr(type->u.iface.name);
		return dup_cstr("<object>");
	}

	if (type->kind == TYPE_UNION) {
		size_t size = 1;
		for (int i = 0; i < type->u.union_type.type_count; i++) {
			char* s = type_to_string(type->u.union_type.types[i]);
			size += strlen(s) + 3;
			free(s);
		}
		char* result = malloc(size);
		result[0] = '\0';
		for (int i = 0; i < type->u.union_type.type_count; i++) {
			if (i > 0) strcat(result, " | ");
			char* s = type_to_string(type->u.union_type.types[i]);
			strcat(result, s);
			free(s);
		}
		return result;
	}

	return dup_cstr("<invalid>");
}

char* type_assignability_error(const Type* expected, const Type* actual) {
	if (!expected || !actual) return dup_cstr("null type");

	if (expected->kind == TYPE_INTERFACE && actual->kind == TYPE_INTERFACE) {
		for (int i = 0; i < expected->u.iface.field_count; i++) {
			const InterfaceField* f = &expected->u.iface.fields[i];
			const Type* af = interface_field_type(actual, f->name);
			if (!af) {
				char buf[256];
				snprintf(buf, sizeof(buf), "missing field '%s'", f->name);
				return dup_cstr(buf);
			}
			if (!type_is_assignable(f->type, af)) {
				char* inner = type_assignability_error(f->type, af);
				size_t sz = strlen(f->name) + strlen(inner) + 16;
				char* buf = malloc(sz);
				snprintf(buf, sz, "field '%s': %s", f->name, inner);
				free(inner);
				return buf;
			}
		}
	}

	char* exp = type_to_string(expected);
	char* got = type_to_string(actual);
	size_t sz = strlen(exp) + strlen(got) + 16;
	char* buf = malloc(sz);
	snprintf(buf, sz, "expected %s, got %s", exp, got);
	free(exp);
	free(got);
	return buf;
}

Value* make_null(void) {
	Value* v = malloc(sizeof(Value));
	v->type = make_basic(BASIC_NULL);
	v->u.i = 0;
	return v;
}

Value* make_bool(int b) {
	Value* v = malloc(sizeof(Value));
	v->type = make_basic(BASIC_BOOL);
	v->u.b = b ? 1 : 0;
	return v;
}

Value* make_int(int i) {
	Value* v = malloc(sizeof(Value));
	v->type = make_basic(BASIC_INT);
	v->u.i = i;
	return v;
}

Value* make_float(double f) {
	Value* v = malloc(sizeof(Value));
	v->type = make_basic(BASIC_FLOAT);
	v->u.f = f;
	return v;
}

Value* make_string(char* s) {
	Value* v = malloc(sizeof(Value));
	v->type = make_basic(BASIC_STRING);
	v->u.s = strdup(s);
	return v;
}

Value* make_array_value(Type* t) {
	Value* v = malloc(sizeof(Value));
	v->type = t;
	v->u.arr.data = NULL;
	v->u.arr.len = 0;
	v->u.arr.cap = 0;
	return v;
}

void array_push(Value* arr, Value* elem) {
	if (arr->u.arr.len >= arr->u.arr.cap) {
		int newcap = arr->u.arr.cap == 0 ? 4 : arr->u.arr.cap * 2;
		arr->u.arr.data =
			realloc(arr->u.arr.data, (size_t)newcap * sizeof(Value*));
		arr->u.arr.cap = newcap;
	}
	arr->u.arr.data[arr->u.arr.len++] = elem;
}

Value* make_object(Type* type, ObjectField* fields, int field_count) {
	Value* v = malloc(sizeof(Value));
	v->type = type;
	v->u.object.fields = fields;
	v->u.object.field_count = field_count;
	v->u.object.field_cap = field_count;
	return v;
}

void object_set_field(Value* obj, const char* name, Value* val) {
	for (int i = 0; i < obj->u.object.field_count; i++) {
		if (strcmp(obj->u.object.fields[i].name, name) == 0) {
			obj->u.object.fields[i].value = val;
			return;
		}
	}
	if (obj->u.object.field_count >= obj->u.object.field_cap) {
		int new_cap =
			obj->u.object.field_cap == 0 ? 4 : obj->u.object.field_cap * 2;
		obj->u.object.fields = realloc(obj->u.object.fields,
									   (size_t)new_cap * sizeof(ObjectField));
		obj->u.object.field_cap = new_cap;
	}
	int i = obj->u.object.field_count++;
	obj->u.object.fields[i].name = strdup(name);
	obj->u.object.fields[i].value = val;
}

void object_delete_field(Value* obj, const char* name) {
	for (int i = 0; i < obj->u.object.field_count; i++) {
		if (strcmp(obj->u.object.fields[i].name, name) == 0) {
			free(obj->u.object.fields[i].name);
			for (int j = i; j < obj->u.object.field_count - 1; j++)
				obj->u.object.fields[j] = obj->u.object.fields[j + 1];
			obj->u.object.field_count--;
			return;
		}
	}
}

void object_clear(Value* obj) {
	for (int i = 0; i < obj->u.object.field_count; i++)
		free(obj->u.object.fields[i].name);
	obj->u.object.field_count = 0;
}

Value* object_get_field(Value* object, const char* field_name) {
	if (!object) return NULL;
	for (int i = 0; i < object->u.object.field_count; i++) {
		if (strcmp(object->u.object.fields[i].name, field_name) == 0)
			return object->u.object.fields[i].value;
	}
	return NULL;
}

char* value_to_string(const Value* value) {
	if (!value || !value->type) return dup_cstr("<null>");

	if (value->type->kind == TYPE_BASIC) {
		switch (value->type->u.basic) {
			case BASIC_NULL:
				return dup_cstr("null");
			case BASIC_BOOL:
				return dup_cstr(value->u.b ? "true" : "false");
			case BASIC_INT: {
				char buffer[32];
				snprintf(buffer, sizeof(buffer), "%d", value->u.i);
				return dup_cstr(buffer);
			}
			case BASIC_FLOAT: {
				char buffer[64];
				snprintf(buffer, sizeof(buffer), "%g", value->u.f);
				return dup_cstr(buffer);
			}
			case BASIC_STRING:
				return dup_cstr(value->u.s ? value->u.s : "");
			case BASIC_ANY:
				return dup_cstr("<any>");
		}
	}

	if (value->type->kind == TYPE_FUNC) return dup_cstr("<function>");

	if (value->type->kind == TYPE_ARRAY) {
		/* Build "[elem0, elem1, ...]" */
		int len = value->u.arr.len;
		if (len == 0) return dup_cstr("[]");

		/* Collect element strings and total length */
		char** parts = malloc((size_t)len * sizeof(char*));
		size_t total = 2; /* "[]" */
		for (int i = 0; i < len; i++) {
			parts[i] = value_to_string(value->u.arr.data[i]);
			total += strlen(parts[i]);
			if (i < len - 1) total += 2; /* ", " */
		}

		char* buf = malloc(total + 1);
		buf[0] = '[';
		size_t off = 1;
		for (int i = 0; i < len; i++) {
			size_t plen = strlen(parts[i]);
			memcpy(buf + off, parts[i], plen);
			off += plen;
			free(parts[i]);
			if (i < len - 1) {
				buf[off++] = ',';
				buf[off++] = ' ';
			}
		}
		buf[off++] = ']';
		buf[off] = '\0';
		free(parts);
		return buf;
	}

	if (value->type->kind == TYPE_INTERFACE) return dup_cstr("<object>");
	return dup_cstr("<value>");
}

Env* env_add(Env* env, char* name, Value* val) {
	Env* e = malloc(sizeof(Env));
	e->name = strdup(name);
	e->value = val;
	e->next = env;
	e->is_const = 0;
	return e;
}

Env* env_add_const(Env* env, char* name, Value* val) {
	Env* e = env_add(env, name, val);
	e->is_const = 1;
	return e;
}

Value* env_get(Env* env, char* name) {
	for (Env* e = env; e; e = e->next) {
		if (e->name && strcmp(e->name, name) == 0) return e->value;
	}
	return NULL;
}

/* Returns: 1=success, 0=not found, -1=const violation */
int env_set(Env* env, char* name, Value* val) {
	for (Env* e = env; e; e = e->next) {
		if (e->name && strcmp(e->name, name) == 0) {
			if (e->is_const) return -1;
			e->value = val;
			return 1;
		}
	}
	return 0;
}

Env* env_push_scope(Env* env) {
	Env* e = malloc(sizeof(Env));
	e->name = NULL;
	e->value = NULL;
	e->next = env;
	return e;
}

int env_has_local(Env* env, char* name) {
	for (Env* e = env; e; e = e->next) {
		if (!e->name) return 0;
		if (strcmp(e->name, name) == 0) return 1;
	}
	return 0;
}
