#include "builtins.h"
#include "vm.h"

#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Builtin error channel ───────────────────────────────────── */

static char g_builtin_error[512];

void builtin_set_error(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_builtin_error, sizeof(g_builtin_error), fmt, ap);
	va_end(ap);
}

const char* builtin_get_error(void) {
	return g_builtin_error[0] ? g_builtin_error : NULL;
}

void builtin_clear_error(void) {
	g_builtin_error[0] = '\0';
}

/* ── Builtin functions ───────────────────────────────────────── */

Value* builtin_console_write(Value** args, int n) {
	for (int i = 0; i < n; i++) {
		char* s = value_to_string(args[i]);
		if (i > 0) printf(" ");
		printf("%s", s);
		free(s);
	}
	return make_null();
}

Value* builtin_console_writeln(Value** args, int n) {
	builtin_console_write(args, n);
	printf("\n");
	return make_null();
}

Value* builtin_console_readln(Value** args, int n) {
	char* line = NULL;
	size_t len = 0;
	ssize_t read = getline(&line, &len, stdin);
	if (read == -1) {
		free(line);
		return make_string("");
	}
	// Remove trailing newline
	if (read > 0 && line[read - 1] == '\n') {
		line[read - 1] = '\0';
	}
	Value* result = make_string(line);
	free(line);
	return result;
}

Value* builtin_console_read(Value** args, int n) {
	int ch = getchar();
	if (ch == EOF) {
		return make_string("");
	}

	// Handle UTF-8: determine how many bytes this character uses
	int bytes_needed = 1;
	if ((ch & 0x80) == 0) {
		// ASCII character (0xxxxxxx)
		bytes_needed = 1;
	} else if ((ch & 0xE0) == 0xC0) {
		// 2-byte sequence (110xxxxx)
		bytes_needed = 2;
	} else if ((ch & 0xF0) == 0xE0) {
		// 3-byte sequence (1110xxxx)
		bytes_needed = 3;
	} else if ((ch & 0xF8) == 0xF0) {
		// 4-byte sequence (11110xxx)
		bytes_needed = 4;
	} else {
		// Invalid UTF-8, treat as single byte
		bytes_needed = 1;
	}

	char* utf8_char = malloc((size_t)bytes_needed + 1);
	utf8_char[0] = (char)ch;

	// Read remaining bytes
	for (int i = 1; i < bytes_needed; i++) {
		int next_ch = getchar();
		if (next_ch == EOF) {
			// Incomplete UTF-8 sequence, return what we have
			utf8_char[i] = '\0';
			Value* result = make_string(utf8_char);
			free(utf8_char);
			return result;
		}
		utf8_char[i] = (char)next_ch;
	}

	utf8_char[bytes_needed] = '\0';
	Value* result = make_string(utf8_char);
	free(utf8_char);
	return result;
}

/* ── Array builtins ─────────────────────────────────────────── */

Value* builtin_array_push(Value** args, int n) {
	if (n < 2 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	Type* elem_type = args[0]->type->u.array.elem;
	for (int i = 1; i < n; i++) {
		if (!type_is_assignable(elem_type, args[i]->type)) {
			char* exp = type_to_string(elem_type);
			char* got = type_to_string(args[i]->type);
			builtin_set_error(
				"Type error: array.push: expected %s, got %s", exp, got);
			free(exp);
			free(got);
			return NULL;
		}
		array_push(args[0], args[i]);
	}
	return args[0];
}

Value* builtin_array_pop(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	if (args[0]->u.arr.len == 0) return make_null();
	return args[0]->u.arr.data[--args[0]->u.arr.len];
}

Value* builtin_array_length(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_int(0);
	return make_int(args[0]->u.arr.len);
}

Value* builtin_array_get(Value** args, int n) {
	if (n < 2 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	int i = args[1]->u.i;
	if (i < 0 || i >= args[0]->u.arr.len) return make_null();
	return args[0]->u.arr.data[i];
}

Value* builtin_array_set(Value** args, int n) {
	if (n < 3 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	int i = args[1]->u.i;
	if (i < 0 || i >= args[0]->u.arr.len) return make_null();
	Type* elem_type = args[0]->type->u.array.elem;
	if (!type_is_assignable(elem_type, args[2]->type)) {
		char* exp = type_to_string(elem_type);
		char* got = type_to_string(args[2]->type);
		builtin_set_error(
			"Type error: array.set: expected %s, got %s", exp, got);
		free(exp);
		free(got);
		return NULL;
	}
	args[0]->u.arr.data[i] = args[2];
	return args[2];
}

Value* builtin_array_copy(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	Value* src = args[0];
	Value* dst = make_array_value(src->type);
	for (int i = 0; i < src->u.arr.len; i++)
		array_push(dst, src->u.arr.data[i]);
	return dst;
}

static int value_is_truthy(Value* v) {
	if (!v) return 0;
	if (v->type->kind != TYPE_BASIC) return 1;
	switch (v->type->u.basic) {
		case BASIC_NULL:
			return 0;
		case BASIC_BOOL:
			return v->u.b;
		case BASIC_INT:
			return v->u.i != 0;
		case BASIC_FLOAT:
			return v->u.f != 0.0;
		case BASIC_STRING:
			return v->u.s && v->u.s[0] != '\0';
		default:
			return 1;
	}
}

Value* builtin_array_sort(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_ARRAY)
		return make_null();
	Value* arr = args[0];
	Value* cmp = (n >= 2 && args[1] && args[1]->type->kind == TYPE_FUNC)
					 ? args[1]
					 : NULL;
	VM* vm = vm_get_global();
	int len = arr->u.arr.len;

	for (int i = 1; i < len; i++) {
		Value* key = arr->u.arr.data[i];
		int j = i - 1;
		while (j >= 0) {
			int key_before_j;
			if (cmp) {
				Value* cargs[2] = { key, arr->u.arr.data[j] };
				Value* result = vm_call_value(vm, cmp, cargs, 2);
				if (!result) {
					builtin_set_error(
						"Runtime error: array.sort comparator failed");
					return NULL;
				}
				key_before_j = value_is_truthy(result);
			} else {
				Value* a = key;
				Value* b = arr->u.arr.data[j];
				if (a->type->kind != TYPE_BASIC || b->type->kind != 0) {
					builtin_set_error("Type error: array.sort: default "
									  "comparator expects numbers");
					return NULL;
				}
				double da =
					a->type->u.basic == BASIC_FLOAT ? a->u.f : (double)a->u.i;
				double db =
					b->type->u.basic == BASIC_FLOAT ? b->u.f : (double)b->u.i;
				key_before_j = da < db;
			}
			if (key_before_j) {
				arr->u.arr.data[j + 1] = arr->u.arr.data[j];
				j--;
			} else {
				break;
			}
		}
		arr->u.arr.data[j + 1] = key;
	}
	return arr;
}

/* ── Struct builtins ────────────────────────────────────────── */

Value* builtin_struct_set(Value** args, int n) {
	if (n < 3 || !args[0] || args[0]->type->kind != TYPE_INTERFACE)
		return make_null();
	if (!args[1] || args[1]->type->kind != TYPE_BASIC
		|| args[1]->type->u.basic != BASIC_STRING)
		return make_null();
	object_set_field(args[0], args[1]->u.s, args[2]);
	return args[0];
}

Value* builtin_struct_get(Value** args, int n) {
	if (n < 2 || !args[0] || args[0]->type->kind != TYPE_INTERFACE)
		return make_null();
	if (!args[1] || args[1]->type->kind != TYPE_BASIC
		|| args[1]->type->u.basic != BASIC_STRING)
		return make_null();
	Value* field = object_get_field(args[0], args[1]->u.s);
	return field ? field : make_null();
}

Value* builtin_struct_delete(Value** args, int n) {
	if (n < 2 || !args[0] || args[0]->type->kind != TYPE_INTERFACE)
		return make_null();
	if (!args[1] || args[1]->type->kind != TYPE_BASIC
		|| args[1]->type->u.basic != BASIC_STRING)
		return make_null();
	object_delete_field(args[0], args[1]->u.s);
	return make_null();
}

Value* builtin_struct_clear(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_INTERFACE)
		return make_null();
	object_clear(args[0]);
	return make_null();
}

Value* builtin_struct_get_keys(Value** args, int n) {
	Value* arr = make_array_value(make_array(make_basic(BASIC_STRING)));
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_INTERFACE) return arr;
	for (int i = 0; i < args[0]->u.object.field_count; i++)
		array_push(arr, make_string(args[0]->u.object.fields[i].name));
	return arr;
}

Value* builtin_struct_get_values(Value** args, int n) {
	Value* arr = make_array_value(make_array(make_basic(BASIC_ANY)));
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_INTERFACE) return arr;
	for (int i = 0; i < args[0]->u.object.field_count; i++)
		array_push(arr, args[0]->u.object.fields[i].value);
	return arr;
}

/* ── JSON Builtins ───────────────────────────────────────────── */

typedef struct {
	char* buf;
	size_t len;
	size_t cap;
} SB;

static void sb_init(SB* sb) {
	sb->cap = 64;
	sb->len = 0;
	sb->buf = malloc(sb->cap);
	sb->buf[0] = '\0';
}

static void sb_grow(SB* sb, size_t extra) {
	while (sb->len + extra + 1 > sb->cap) {
		sb->cap *= 2;
		sb->buf = realloc(sb->buf, sb->cap);
	}
}

static void sb_char(SB* sb, char c) {
	sb_grow(sb, 1);
	sb->buf[sb->len++] = c;
	sb->buf[sb->len] = '\0';
}

static void sb_str(SB* sb, const char* s) {
	size_t n = strlen(s);
	sb_grow(sb, n);
	memcpy(sb->buf + sb->len, s, n + 1);
	sb->len += n;
}

static void json_val_to_string(SB* sb, Value* v) {
	if (!v) {
		sb_str(sb, "null");
		return;
	}
	if (v->type->kind == TYPE_BASIC) {
		switch (v->type->u.basic) {
			case BASIC_NULL:
				sb_str(sb, "null");
				break;
			case BASIC_BOOL:
				sb_str(sb, v->u.b ? "true" : "false");
				break;
			case BASIC_INT: {
				char tmp[32];
				snprintf(tmp, sizeof(tmp), "%d", v->u.i);
				sb_str(sb, tmp);
				break;
			}
			case BASIC_FLOAT: {
				char tmp[64];
				snprintf(tmp, sizeof(tmp), "%g", v->u.f);
				sb_str(sb, tmp);
				break;
			}
			case BASIC_STRING: {
				sb_char(sb, '"');
				for (const char* p = v->u.s; *p; p++) {
					if (*p == '"')
						sb_str(sb, "\\\"");
					else if (*p == '\\')
						sb_str(sb, "\\\\");
					else if (*p == '\n')
						sb_str(sb, "\\n");
					else if (*p == '\r')
						sb_str(sb, "\\r");
					else if (*p == '\t')
						sb_str(sb, "\\t");
					else
						sb_char(sb, *p);
				}
				sb_char(sb, '"');
				break;
			}
			default:
				sb_str(sb, "null");
		}
	} else if (v->type->kind == TYPE_ARRAY) {
		sb_char(sb, '[');
		for (int i = 0; i < v->u.arr.len; i++) {
			if (i) sb_char(sb, ',');
			json_val_to_string(sb, v->u.arr.data[i]);
		}
		sb_char(sb, ']');
	} else if (v->type->kind == TYPE_INTERFACE) {
		sb_char(sb, '{');
		for (int i = 0; i < v->u.object.field_count; i++) {
			if (i) sb_char(sb, ',');
			sb_char(sb, '"');
			sb_str(sb, v->u.object.fields[i].name);
			sb_char(sb, '"');
			sb_char(sb, ':');
			json_val_to_string(sb, v->u.object.fields[i].value);
		}
		sb_char(sb, '}');
	} else {
		sb_str(sb, "null");
	}
}

Value* builtin_json_stringify(Value** args, int n) {
	if (n < 1 || !args[0]) return make_string("null");
	SB sb;
	sb_init(&sb);
	json_val_to_string(&sb, args[0]);
	Value* result = make_string(sb.buf);
	free(sb.buf);
	return result;
}

typedef struct {
	const char* src;
	size_t pos;
} JP;

static Value* jp_parse(JP* jp);

static void jp_skip_ws(JP* jp) {
	char c;
	while ((c = jp->src[jp->pos]) == ' ' || c == '\t' || c == '\n' || c == '\r')
		jp->pos++;
}

static Value* jp_parse_string(JP* jp) {
	jp->pos++;
	SB sb;
	sb_init(&sb);
	while (jp->src[jp->pos] && jp->src[jp->pos] != '"') {
		if (jp->src[jp->pos] == '\\') {
			jp->pos++;
			switch (jp->src[jp->pos]) {
				case '"':
					sb_char(&sb, '"');
					break;
				case '\\':
					sb_char(&sb, '\\');
					break;
				case 'n':
					sb_char(&sb, '\n');
					break;
				case 'r':
					sb_char(&sb, '\r');
					break;
				case 't':
					sb_char(&sb, '\t');
					break;
				default:
					sb_char(&sb, jp->src[jp->pos]);
					break;
			}
		} else {
			sb_char(&sb, jp->src[jp->pos]);
		}
		jp->pos++;
	}
	if (jp->src[jp->pos] == '"') jp->pos++;
	Value* result = make_string(sb.buf);
	free(sb.buf);
	return result;
}

static Value* jp_parse_array(JP* jp) {
	jp->pos++;
	Value* arr = make_array_value(make_array(make_basic(BASIC_ANY)));
	jp_skip_ws(jp);
	while (jp->src[jp->pos] && jp->src[jp->pos] != ']') {
		Value* elem = jp_parse(jp);
		if (!elem) break;
		array_push(arr, elem);
		jp_skip_ws(jp);
		if (jp->src[jp->pos] == ',') jp->pos++;
	}
	if (jp->src[jp->pos] == ']') jp->pos++;
	return arr;
}

static Value* jp_parse_object(JP* jp) {
	jp->pos++;
	int cap = 8;
	ObjectField* fields = malloc((size_t)cap * sizeof(ObjectField));
	InterfaceField* ifields = malloc((size_t)cap * sizeof(InterfaceField));
	int count = 0;
	jp_skip_ws(jp);
	while (jp->src[jp->pos] && jp->src[jp->pos] != '}') {
		jp_skip_ws(jp);
		if (jp->src[jp->pos] != '"') break;
		Value* key = jp_parse_string(jp);
		jp_skip_ws(jp);
		if (jp->src[jp->pos] != ':') break;
		jp->pos++;
		jp_skip_ws(jp);
		Value* val = jp_parse(jp);
		if (!val) break;
		if (count >= cap) {
			cap *= 2;
			fields = realloc(fields, (size_t)cap * sizeof(ObjectField));
			ifields = realloc(ifields, (size_t)cap * sizeof(InterfaceField));
		}
		fields[count].name = strdup(key->u.s);
		fields[count].value = val;
		ifields[count].name = strdup(key->u.s);
		ifields[count].type = val->type;
		count++;
		jp_skip_ws(jp);
		if (jp->src[jp->pos] == ',') jp->pos++;
	}
	if (jp->src[jp->pos] == '}') jp->pos++;
	Type* obj_type = make_interface(NULL, ifields, count);
	return make_object(obj_type, fields, count);
}

static Value* jp_parse(JP* jp) {
	jp_skip_ws(jp);
	char c = jp->src[jp->pos];
	if (c == '"') return jp_parse_string(jp);
	if (c == '[') return jp_parse_array(jp);
	if (c == '{') return jp_parse_object(jp);
	if (c == 't' && strncmp(jp->src + jp->pos, "true", 4) == 0) {
		jp->pos += 4;
		return make_bool(1);
	}
	if (c == 'f' && strncmp(jp->src + jp->pos, "false", 5) == 0) {
		jp->pos += 5;
		return make_bool(0);
	}
	if (c == 'n' && strncmp(jp->src + jp->pos, "null", 4) == 0) {
		jp->pos += 4;
		return make_null();
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		char* end;
		long long iv = strtoll(jp->src + jp->pos, &end, 10);
		if (*end != '.' && *end != 'e' && *end != 'E') {
			jp->pos = (size_t)(end - jp->src);
			return make_int((int)iv);
		}
		double fv = strtod(jp->src + jp->pos, &end);
		jp->pos = (size_t)(end - jp->src);
		return make_float(fv);
	}
	return make_null();
}

Value* builtin_json_parse(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_null();
	JP jp = { args[0]->u.s, 0 };
	return jp_parse(&jp);
}

/* ── Cast Builtins ───────────────────────────────────────────── */

Value* builtin_cast_to_bool(Value** args, int n) {
	if (n < 1 || !args[0]) return make_bool(0);
	Value* v = args[0];
	if (v->type->kind != TYPE_BASIC) return make_bool(1);
	switch (v->type->u.basic) {
		case BASIC_NULL:
			return make_bool(0);
		case BASIC_BOOL:
			return make_bool(v->u.b);
		case BASIC_INT:
			return make_bool(v->u.i != 0);
		case BASIC_FLOAT:
			return make_bool(v->u.f != 0.0);
		case BASIC_STRING:
			return make_bool(v->u.s && v->u.s[0] != '\0');
		default:
			return make_bool(1);
	}
}

Value* builtin_cast_to_string(Value** args, int n) {
	if (n < 1 || !args[0]) return make_string("null");
	char* s = value_to_string(args[0]);
	Value* result = make_string(s);
	free(s);
	return result;
}

Value* builtin_cast_to_int(Value** args, int n) {
	if (n < 1 || !args[0]) return make_int(0);
	Value* v = args[0];
	if (v->type->kind != TYPE_BASIC) return make_int(0);
	switch (v->type->u.basic) {
		case BASIC_NULL:
			return make_int(0);
		case BASIC_BOOL:
			return make_int(v->u.b ? 1 : 0);
		case BASIC_INT:
			return make_int(v->u.i);
		case BASIC_FLOAT:
			return make_int((int)v->u.f);
		case BASIC_STRING:
			return make_int(v->u.s ? (int)strtol(v->u.s, NULL, 10) : 0);
		default:
			return make_int(0);
	}
}

Value* builtin_cast_to_float(Value** args, int n) {
	if (n < 1 || !args[0]) return make_float(0.0);
	Value* v = args[0];
	if (v->type->kind != TYPE_BASIC) return make_float(0.0);
	switch (v->type->u.basic) {
		case BASIC_NULL:
			return make_float(0.0);
		case BASIC_BOOL:
			return make_float(v->u.b ? 1.0 : 0.0);
		case BASIC_INT:
			return make_float((double)v->u.i);
		case BASIC_FLOAT:
			return make_float(v->u.f);
		case BASIC_STRING:
			return make_float(v->u.s ? strtod(v->u.s, NULL) : 0.0);
		default:
			return make_float(0.0);
	}
}

/* ── String Builtins ─────────────────────────────────────────── */

static int utf8_charlen(const char* s) {
	int len = 0;
	while (*s) {
		if ((*s & 0xC0) != 0x80) len++;
		s++;
	}
	return len;
}

static char* translate_regex_pattern(const char* pattern) {
	SB sb;
	sb_init(&sb);
	for (const char* p = pattern; *p; p++) {
		if (*p == '\\' && *(p + 1)) {
			p++;
			switch (*p) {
				case 's':
					sb_str(&sb, "[[:space:]]");
					break;
				case 'S':
					sb_str(&sb, "[^[:space:]]");
					break;
				case 'd':
					sb_str(&sb, "[0-9]");
					break;
				case 'D':
					sb_str(&sb, "[^0-9]");
					break;
				case 'w':
					sb_str(&sb, "[[:alnum:]_]");
					break;
				case 'W':
					sb_str(&sb, "[^[:alnum:]_]");
					break;
				default: {
					char tmp[3] = { '\\', *p, '\0' };
					sb_str(&sb, tmp);
					break;
				}
			}
		} else {
			sb_char(&sb, *p);
		}
	}
	return sb.buf;
}

Value* builtin_string_length(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_int(0);
	return make_int(utf8_charlen(args[0]->u.s));
}

Value* builtin_string_lowercase(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_string("");
	const char* s = args[0]->u.s;
	size_t len = strlen(s);
	char* result = malloc(len + 1);
	for (size_t i = 0; i <= len; i++)
		result[i] = (char)tolower((unsigned char)s[i]);
	Value* v = make_string(result);
	free(result);
	return v;
}

Value* builtin_string_uppercase(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_string("");
	const char* s = args[0]->u.s;
	size_t len = strlen(s);
	char* result = malloc(len + 1);
	for (size_t i = 0; i <= len; i++)
		result[i] = (char)toupper((unsigned char)s[i]);
	Value* v = make_string(result);
	free(result);
	return v;
}

Value* builtin_string_trim(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_string("");
	const char* s = args[0]->u.s;
	while (*s && isspace((unsigned char)*s))
		s++;
	const char* end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end - 1)))
		end--;
	size_t len = (size_t)(end - s);
	char* result = malloc(len + 1);
	memcpy(result, s, len);
	result[len] = '\0';
	Value* v = make_string(result);
	free(result);
	return v;
}

Value* builtin_string_split(Value** args, int n) {
	Value* result = make_array_value(make_array(make_basic(BASIC_STRING)));
	if (n < 2 || !args[0] || !args[1] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING
		|| args[1]->type->kind != TYPE_BASIC
		|| args[1]->type->u.basic != BASIC_STRING)
		return result;

	const char* str = args[0]->u.s;
	char* pat = translate_regex_pattern(args[1]->u.s);
	regex_t re;

	if (regcomp(&re, pat, REG_EXTENDED) != 0) {
		free(pat);
		array_push(result, make_string((char*)str));
		return result;
	}
	free(pat);

	const char* pos = str;
	regmatch_t m;

	while (1) {
		if (regexec(&re, pos, 1, &m, 0) != 0) {
			array_push(result, make_string((char*)pos));
			break;
		}
		size_t part_len = (size_t)m.rm_so;
		char* part = malloc(part_len + 1);
		memcpy(part, pos, part_len);
		part[part_len] = '\0';
		array_push(result, make_string(part));
		free(part);
		pos += m.rm_eo > 0 ? (size_t)m.rm_eo : 1;
	}

	regfree(&re);
	return result;
}

Value* builtin_string_is_bool(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_bool(0);
	const char* s = args[0]->u.s;
	return make_bool(strcmp(s, "true") == 0 || strcmp(s, "false") == 0);
}

Value* builtin_string_is_int(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_bool(0);
	const char* s = args[0]->u.s;
	if (!*s) return make_bool(0);
	if (*s == '-') s++;
	if (!*s) return make_bool(0);
	while (*s) {
		if (!isdigit((unsigned char)*s)) return make_bool(0);
		s++;
	}
	return make_bool(1);
}

Value* builtin_string_is_float(Value** args, int n) {
	if (n < 1 || !args[0] || args[0]->type->kind != TYPE_BASIC
		|| args[0]->type->u.basic != BASIC_STRING)
		return make_bool(0);
	const char* s = args[0]->u.s;
	if (!*s) return make_bool(0);
	char* end;
	strtod(s, &end);
	return make_bool(end != s && *end == '\0');
}

/* ── Type Builtins ───────────────────────────────────────────── */

Value* builtin_type_of(Value** args, int n) {
	if (n < 1 || !args[0]) return make_string("null");
	Type* t = args[0]->declared_type ? args[0]->declared_type : args[0]->type;
	char* s = type_to_string(t);
	Value* result = make_string(s);
	free(s);
	return result;
}

/* ── System Builtins ─────────────────────────────────────────── */

Value* builtin_system_quit(Value** args, int n) {
	int code = 0;
	if (n > 0 && args[0]->type->kind == TYPE_BASIC
		&& args[0]->type->u.basic == BASIC_INT)
		code = args[0]->u.i;
	exit(code);
}

Value* builtin_system_args(Value** args, int n) {
	extern int global_argc;
	extern char** global_argv;

	Type* elem_type = make_basic(BASIC_STRING);
	Value* arr = make_array_value(make_array(elem_type));

	for (int i = 0; i < global_argc; i++) {
		Value* str = make_string(global_argv[i]);
		array_push(arr, str);
	}
	return arr;
}

const BuiltinEntry BUILTINS[] = {
	// console
	{ "__builtin_console_writeln__", builtin_console_writeln },
	{ "__builtin_console_write__",   builtin_console_write	 },
	{ "__builtin_console_read__",	  builtin_console_read	   },
	{ "__builtin_console_readln__",	builtin_console_readln  },
	// system
	{ "__builtin_system_quit__",	 builtin_system_quit	 },
	{ "__builtin_system_args__",	 builtin_system_args	 },
	// array
	{ "__builtin_array_push__",		builtin_array_push	   },
	{ "__builtin_array_pop__",	   builtin_array_pop		 },
	{ "__builtin_array_length__",	  builtin_array_length	   },
	{ "__builtin_array_get__",	   builtin_array_get		 },
	{ "__builtin_array_set__",	   builtin_array_set		 },
	{ "__builtin_array_copy__",		builtin_array_copy	   },
	// json
	{ "__builtin_json_stringify__",	builtin_json_stringify  },
	{ "__builtin_json_parse__",		builtin_json_parse	   },
	// type
	{ "__builtin_type_of__",		 builtin_type_of		 },
	// cast
	{ "__builtin_cast_to_bool__",	  builtin_cast_to_bool	   },
	{ "__builtin_cast_to_string__",	builtin_cast_to_string  },
	{ "__builtin_cast_to_int__",	 builtin_cast_to_int	 },
	{ "__builtin_cast_to_float__",   builtin_cast_to_float	 },
	{ NULL,						  NULL					},
};

VMBuiltinFn builtin_lookup(const char* sentinel) {
	for (int i = 0; BUILTINS[i].sentinel; i++)
		if (strcmp(BUILTINS[i].sentinel, sentinel) == 0) return BUILTINS[i].fn;
	return NULL;
}

Value* make_builtin_closure(VMBuiltinFn fn, Type* type) {
	Closure* cl = malloc(sizeof(Closure));
	cl->chunk = NULL;
	cl->env = NULL;
	cl->is_builtin = 1;
	cl->builtin_fn = fn;
	Value* v = malloc(sizeof(Value));
	v->type = type;
	v->u.closure = cl;
	return v;
}

Env* make_builtin_exports(void) {
	// Console object
	Type* any_arr_type = make_array(make_basic(BASIC_ANY));
	Type** write_params = malloc(sizeof(Type*));
	write_params[0] = any_arr_type;
	Type* write_type = make_func(make_basic(BASIC_NULL), write_params, 1);
	write_type->u.func.is_variadic = 1;
	free(write_params);
	Type* writeln_type = make_func(make_basic(BASIC_NULL), &any_arr_type, 1);
	writeln_type->u.func.is_variadic = 1;

	ObjectField* console_fields = malloc(4 * sizeof(ObjectField));
	console_fields[0].name = strdup("write");
	console_fields[0].value = make_builtin_closure(builtin_console_write, write_type);
	console_fields[1].name = strdup("writeln");
	console_fields[1].value = make_builtin_closure(builtin_console_writeln, writeln_type);
	console_fields[2].name = strdup("read");
	console_fields[2].value = make_builtin_closure(
		builtin_console_read, make_func(make_basic(BASIC_STRING), NULL, 0));
	console_fields[3].name = strdup("readln");
	console_fields[3].value = make_builtin_closure(
		builtin_console_readln, make_func(make_basic(BASIC_STRING), NULL, 0));
	InterfaceField* console_iface_fields = malloc(4 * sizeof(InterfaceField));
	console_iface_fields[0].name = strdup("write");
	console_iface_fields[0].type = write_type;
	console_iface_fields[1].name = strdup("writeln");
	console_iface_fields[1].type = writeln_type;
	console_iface_fields[2].name = strdup("read");
	console_iface_fields[2].type = make_func(make_basic(BASIC_STRING), NULL, 0);
	console_iface_fields[3].name = strdup("readln");
	console_iface_fields[3].type = make_func(make_basic(BASIC_STRING), NULL, 0);
	Type* console_type = make_interface(NULL, console_iface_fields, 4);
	Value* console_obj = make_object(console_type, console_fields, 4);

	// System object
	ObjectField* system_fields = malloc(2 * sizeof(ObjectField));
	system_fields[0].name = strdup("quit");
	Type* quit_params[1] = { make_basic(BASIC_INT) };
	system_fields[0].value = make_builtin_closure(
		builtin_system_quit, make_func(make_basic(BASIC_NULL), quit_params, 1));
	system_fields[1].name = strdup("args");
	Type* string_arr = make_array(make_basic(BASIC_STRING));
	system_fields[1].value = make_builtin_closure(
		builtin_system_args, make_func(string_arr, NULL, 0));
	InterfaceField* system_iface_fields = malloc(2 * sizeof(InterfaceField));
	system_iface_fields[0].name = strdup("quit");
	system_iface_fields[0].type =
		make_func(make_basic(BASIC_NULL), quit_params, 1);
	system_iface_fields[1].name = strdup("args");
	system_iface_fields[1].type = make_func(string_arr, NULL, 0);
	Type* system_type = make_interface(NULL, system_iface_fields, 2);
	Value* system_obj = make_object(system_type, system_fields, 2);

	/* Array object: push, pop, len, get, set */
	Type* any_arr = make_array(make_basic(BASIC_ANY));
	Type* any_t = make_basic(BASIC_ANY);
	Type* int_t = make_basic(BASIC_INT);
	Type* null_t = make_basic(BASIC_NULL);

	ObjectField* af = malloc(7 * sizeof(ObjectField));
	InterfaceField* aif = malloc(7 * sizeof(InterfaceField));
	int afc = 0;
	(void)any_arr;
	(void)any_t;
	(void)int_t;
	(void)null_t;

	{
		Type* _p[] = { any_arr, any_t };
		af[afc].name = strdup("push");
		af[afc].value =
			make_builtin_closure(builtin_array_push, make_func(any_arr, _p, 2));
		aif[afc].name = strdup("push");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr };
		af[afc].name = strdup("pop");
		af[afc].value =
			make_builtin_closure(builtin_array_pop, make_func(any_t, _p, 1));
		aif[afc].name = strdup("pop");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr };
		af[afc].name = strdup("length");
		af[afc].value =
			make_builtin_closure(builtin_array_length, make_func(int_t, _p, 1));
		aif[afc].name = strdup("length");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr, int_t };
		af[afc].name = strdup("get");
		af[afc].value =
			make_builtin_closure(builtin_array_get, make_func(any_t, _p, 2));
		aif[afc].name = strdup("get");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr, int_t, any_t };
		af[afc].name = strdup("set");
		af[afc].value =
			make_builtin_closure(builtin_array_set, make_func(null_t, _p, 3));
		aif[afc].name = strdup("set");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr };
		af[afc].name = strdup("sort");
		af[afc].value =
			make_builtin_closure(builtin_array_sort, make_func(any_arr, _p, 1));
		aif[afc].name = strdup("sort");
		aif[afc].type = af[afc].value->type;
		afc++;
	}
	{
		Type* _p[] = { any_arr };
		af[afc].name = strdup("copy");
		af[afc].value =
			make_builtin_closure(builtin_array_copy, make_func(any_arr, _p, 1));
		aif[afc].name = strdup("copy");
		aif[afc].type = af[afc].value->type;
		afc++;
	}

	Type* array_type = make_interface(NULL, aif, afc);
	Value* array_obj = make_object(array_type, af, afc);

	/* Struct object: set, get, delete, clear, get_keys, get_values */
	Type* any2 = make_basic(BASIC_ANY);
	Type* str2 = make_basic(BASIC_STRING);
	Type* null2 = make_basic(BASIC_NULL);
	Type* str_arr = make_array(make_basic(BASIC_STRING));
	Type* any_arr2 = make_array(make_basic(BASIC_ANY));

	ObjectField* sf = malloc(6 * sizeof(ObjectField));
	InterfaceField* sif = malloc(6 * sizeof(InterfaceField));
	int sfc = 0;

	{
		Type* _p[] = { any2, str2, any2 };
		sf[sfc].name = strdup("set");
		sf[sfc].value =
			make_builtin_closure(builtin_struct_set, make_func(any2, _p, 3));
		sif[sfc].name = strdup("set");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}
	{
		Type* _p[] = { any2, str2 };
		sf[sfc].name = strdup("get");
		sf[sfc].value =
			make_builtin_closure(builtin_struct_get, make_func(any2, _p, 2));
		sif[sfc].name = strdup("get");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}
	{
		Type* _p[] = { any2, str2 };
		sf[sfc].name = strdup("delete");
		sf[sfc].value = make_builtin_closure(builtin_struct_delete,
											 make_func(null2, _p, 2));
		sif[sfc].name = strdup("delete");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}
	{
		Type* _p[] = { any2 };
		sf[sfc].name = strdup("clear");
		sf[sfc].value =
			make_builtin_closure(builtin_struct_clear, make_func(null2, _p, 1));
		sif[sfc].name = strdup("clear");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}
	{
		Type* _p[] = { any2 };
		sf[sfc].name = strdup("get_keys");
		sf[sfc].value = make_builtin_closure(builtin_struct_get_keys,
											 make_func(str_arr, _p, 1));
		sif[sfc].name = strdup("get_keys");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}
	{
		Type* _p[] = { any2 };
		sf[sfc].name = strdup("get_values");
		sf[sfc].value = make_builtin_closure(builtin_struct_get_values,
											 make_func(any_arr2, _p, 1));
		sif[sfc].name = strdup("get_values");
		sif[sfc].type = sf[sfc].value->type;
		sfc++;
	}

	Type* struct_type = make_interface(NULL, sif, sfc);
	Value* struct_obj = make_object(struct_type, sf, sfc);

	/* JSON object: stringify, parse */
	ObjectField* json_fields = malloc(2 * sizeof(ObjectField));
	Type* stringify_params[1] = { make_basic(BASIC_ANY) };
	json_fields[0].name = strdup("stringify");
	json_fields[0].value = make_builtin_closure(
		builtin_json_stringify,
		make_func(make_basic(BASIC_STRING), stringify_params, 1));
	Type* parse_params[1] = { make_basic(BASIC_STRING) };
	json_fields[1].name = strdup("parse");
	json_fields[1].value = make_builtin_closure(
		builtin_json_parse, make_func(make_basic(BASIC_ANY), parse_params, 1));
	InterfaceField* json_iface_fields = malloc(2 * sizeof(InterfaceField));
	json_iface_fields[0].name = strdup("stringify");
	json_iface_fields[0].type =
		make_func(make_basic(BASIC_STRING), stringify_params, 1);
	json_iface_fields[1].name = strdup("parse");
	json_iface_fields[1].type =
		make_func(make_basic(BASIC_ANY), parse_params, 1);
	Type* json_type = make_interface(NULL, json_iface_fields, 2);
	Value* json_obj = make_object(json_type, json_fields, 2);

	/* Cast object: to_bool, to_string, to_int, to_float */
	Type* cast_any[1] = { make_basic(BASIC_ANY) };
	ObjectField* cast_fields = malloc(4 * sizeof(ObjectField));
	InterfaceField* cast_iface_fields = malloc(4 * sizeof(InterfaceField));

	cast_fields[0].name = strdup("to_bool");
	cast_fields[0].value = make_builtin_closure(
		builtin_cast_to_bool, make_func(make_basic(BASIC_BOOL), cast_any, 1));
	cast_fields[1].name = strdup("to_string");
	cast_fields[1].value =
		make_builtin_closure(builtin_cast_to_string,
							 make_func(make_basic(BASIC_STRING), cast_any, 1));
	cast_fields[2].name = strdup("to_int");
	cast_fields[2].value = make_builtin_closure(
		builtin_cast_to_int, make_func(make_basic(BASIC_INT), cast_any, 1));
	cast_fields[3].name = strdup("to_float");
	cast_fields[3].value = make_builtin_closure(
		builtin_cast_to_float, make_func(make_basic(BASIC_FLOAT), cast_any, 1));

	for (int i = 0; i < 4; i++) {
		cast_iface_fields[i].name = strdup(cast_fields[i].name);
		cast_iface_fields[i].type = cast_fields[i].value->type;
	}
	Type* cast_type = make_interface(NULL, cast_iface_fields, 4);
	Value* cast_obj = make_object(cast_type, cast_fields, 4);

	/* String object: length, lowercase, uppercase, trim, split */
	Type* str3 = make_basic(BASIC_STRING);
	Type* int3 = make_basic(BASIC_INT);
	Type* str_arr3 = make_array(make_basic(BASIC_STRING));

	ObjectField* strf = malloc(8 * sizeof(ObjectField));
	InterfaceField* strif = malloc(8 * sizeof(InterfaceField));
	int strfc = 0;

	{
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("length");
		strf[strfc].value =
			make_builtin_closure(builtin_string_length, make_func(int3, _p, 1));
		strif[strfc].name = strdup("length");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("lowercase");
		strf[strfc].value = make_builtin_closure(builtin_string_lowercase,
												 make_func(str3, _p, 1));
		strif[strfc].name = strdup("lowercase");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("uppercase");
		strf[strfc].value = make_builtin_closure(builtin_string_uppercase,
												 make_func(str3, _p, 1));
		strif[strfc].name = strdup("uppercase");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("trim");
		strf[strfc].value =
			make_builtin_closure(builtin_string_trim, make_func(str3, _p, 1));
		strif[strfc].name = strdup("trim");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* _p[] = { str3, str3 };
		strf[strfc].name = strdup("split");
		strf[strfc].value = make_builtin_closure(builtin_string_split,
												 make_func(str_arr3, _p, 2));
		strif[strfc].name = strdup("split");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* bool3 = make_basic(BASIC_BOOL);
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("is_bool");
		strf[strfc].value = make_builtin_closure(builtin_string_is_bool,
												 make_func(bool3, _p, 1));
		strif[strfc].name = strdup("is_bool");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* bool3 = make_basic(BASIC_BOOL);
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("is_int");
		strf[strfc].value = make_builtin_closure(builtin_string_is_int,
												 make_func(bool3, _p, 1));
		strif[strfc].name = strdup("is_int");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}
	{
		Type* bool3 = make_basic(BASIC_BOOL);
		Type* _p[] = { str3 };
		strf[strfc].name = strdup("is_float");
		strf[strfc].value = make_builtin_closure(builtin_string_is_float,
												 make_func(bool3, _p, 1));
		strif[strfc].name = strdup("is_float");
		strif[strfc].type = strf[strfc].value->type;
		strfc++;
	}

	Type* string_type = make_interface(NULL, strif, strfc);
	Value* string_obj = make_object(string_type, strf, strfc);

	/* Type object: of */
	Type* type_any[1] = { make_basic(BASIC_ANY) };
	ObjectField* type_fields = malloc(1 * sizeof(ObjectField));
	InterfaceField* type_iface_fields = malloc(1 * sizeof(InterfaceField));
	type_fields[0].name = strdup("of");
	type_fields[0].value = make_builtin_closure(
		builtin_type_of, make_func(make_basic(BASIC_STRING), type_any, 1));
	type_iface_fields[0].name = strdup("of");
	type_iface_fields[0].type = type_fields[0].value->type;
	Type* type_type = make_interface(NULL, type_iface_fields, 1);
	Value* type_obj = make_object(type_type, type_fields, 1);

	Env* env = NULL;
	env = env_add(env, "console", console_obj);
	env = env_add(env, "system", system_obj);
	env = env_add(env, "array", array_obj);
	env = env_add(env, "struct", struct_obj);
	env = env_add(env, "json", json_obj);
	env = env_add(env, "cast", cast_obj);
	env = env_add(env, "string", string_obj);
	env = env_add(env, "type", type_obj);
	return env;
}
