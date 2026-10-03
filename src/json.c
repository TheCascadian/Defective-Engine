/* In-house JSON reader and writer. The reader accepts // and block comments and
 * trailing commas because the files are hand-written by mod authors; every node
 * remembers its line so data errors can point at the exact place. */
#include "dfe.h"

typedef struct Parser {
    const char *p, *end;
    int line;
    char *err;
    size_t errcap;
    int err_line;
    int depth;
} Parser;

#define JSON_MAX_DEPTH 64

static void fail(Parser *ps, const char *msg) {
    if (ps->err && ps->err[0] == 0) {
        snprintf(ps->err, ps->errcap, "%s", msg);
        ps->err_line = ps->line;
    }
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == '\n') { ps->line++; ps->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') ps->p++;
        else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '/') {
            while (ps->p < ps->end && *ps->p != '\n') ps->p++;
        } else if (c == '/' && ps->p + 1 < ps->end && ps->p[1] == '*') {
            ps->p += 2;
            while (ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/')) {
                if (*ps->p == '\n') ps->line++;
                ps->p++;
            }
            ps->p += 2;
        } else if ((u8)c == 0xEF && ps->p + 2 < ps->end && (u8)ps->p[1] == 0xBB && (u8)ps->p[2] == 0xBF) {
            ps->p += 3; /* UTF-8 byte order mark written by some Windows editors */
        } else break;
    }
}

static Json *node_new(Parser *ps, JsonType t) {
    Json *j = xcalloc(1, sizeof *j);
    j->type = t;
    j->line = ps->line;
    return j;
}

static void container_push(Json *c, char *key, Json *item) {
    c->items = xrealloc(c->items, (size_t)(c->count + 1) * sizeof(Json *));
    if (c->type == JSON_OBJECT) c->keys = xrealloc(c->keys, (size_t)(c->count + 1) * sizeof(char *));
    c->items[c->count] = item;
    if (c->type == JSON_OBJECT) c->keys[c->count] = key;
    c->count++;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_append(char **out, u32 cp) {
    char *o = *out;
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 63)); }
    else { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
    *out = o;
}

static char *parse_string_raw(Parser *ps) {
    ps->p++; /* opening quote */
    size_t cap = (size_t)(ps->end - ps->p) + 1;
    char *buf = xmalloc(cap), *o = buf;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c == '\n') { ps->line++; }
        if (c != '\\') { *o++ = c; continue; }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            u32 cp = 0;
            for (int i = 0; i < 4 && ps->p < ps->end; i++) {
                int h = hex_val(*ps->p++);
                if (h < 0) { fail(ps, "invalid \\u escape in string"); h = 0; }
                cp = cp * 16 + (u32)h;
            }
            utf8_append(&o, cp);
            break;
        }
        default: *o++ = e; break;
        }
    }
    if (ps->p >= ps->end) { fail(ps, "unterminated string, add the closing quote"); free(buf); return NULL; }
    ps->p++;
    *o = 0;
    return buf;
}

static Json *parse_value(Parser *ps);

static Json *parse_container(Parser *ps, bool is_obj) {
    Json *c = node_new(ps, is_obj ? JSON_OBJECT : JSON_ARRAY);
    char close = is_obj ? '}' : ']';
    ps->p++;
    if (++ps->depth > JSON_MAX_DEPTH) { fail(ps, "nesting deeper than 64 levels"); return c; }
    while (true) {
        skip_ws(ps);
        if (ps->p >= ps->end) { fail(ps, is_obj ? "unterminated object, add a closing }" : "unterminated array, add a closing ]"); break; }
        if (*ps->p == close) { ps->p++; break; }
        char *key = NULL;
        if (is_obj) {
            if (*ps->p != '"') { fail(ps, "expected a quoted key like \"name\": value"); break; }
            key = parse_string_raw(ps);
            if (!key) break;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':' after the key"); free(key); break; }
            ps->p++;
        }
        Json *v = parse_value(ps);
        if (!v) { free(key); break; }
        container_push(c, key, v);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == close) { ps->p++; break; }
        fail(ps, is_obj ? "expected ',' or '}' after a value (missing comma?)" : "expected ',' or ']' after a value (missing comma?)");
        break;
    }
    ps->depth--;
    return c;
}

static Json *parse_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end of file, a value is missing"); return NULL; }
    char c = *ps->p;
    if (c == '{') return parse_container(ps, true);
    if (c == '[') return parse_container(ps, false);
    if (c == '"') {
        Json *j = node_new(ps, JSON_STRING);
        j->str = parse_string_raw(ps);
        if (!j->str) { free(j); return NULL; }
        return j;
    }
    size_t left = (size_t)(ps->end - ps->p);
    if (left >= 4 && !strncmp(ps->p, "true", 4)) { Json *j = node_new(ps, JSON_BOOL); j->boolean = true; ps->p += 4; return j; }
    if (left >= 5 && !strncmp(ps->p, "false", 5)) { Json *j = node_new(ps, JSON_BOOL); ps->p += 5; return j; }
    if (left >= 4 && !strncmp(ps->p, "null", 4)) { Json *j = node_new(ps, JSON_NULL); ps->p += 4; return j; }
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
        char *endp;
        double d = strtod(ps->p, &endp);
        if (endp == ps->p) { fail(ps, "malformed number"); return NULL; }
        Json *j = node_new(ps, JSON_NUMBER);
        j->num = d;
        ps->p = endp;
        return j;
    }
    fail(ps, "unexpected character, expected a value (string, number, true, false, null, [ or {)");
    return NULL;
}

Json *json_parse(const char *text, size_t len, char *err, size_t errcap, int *err_line) {
    Parser ps = {text, text + len, 1, err, errcap, 0, 0};
    if (err) err[0] = 0;
    Json *root = parse_value(&ps);
    if (root && !(err && err[0])) {
        skip_ws(&ps);
        if (ps.p < ps.end) fail(&ps, "extra text after the top-level value");
    }
    if (err && err[0]) {
        if (err_line) *err_line = ps.err_line;
        json_free(root);
        return NULL;
    }
    return root;
}

Json *json_clone(const Json *src) {
    if (!src) return NULL;
    Json *copy = xcalloc(1, sizeof *copy);
    copy->type = src->type;
    copy->line = src->line;
    copy->num = src->num;
    copy->boolean = src->boolean;
    if (src->type == JSON_STRING) copy->str = src->str ? xstrdup(src->str) : xstrdup("");
    else if (src->type == JSON_ARRAY || src->type == JSON_OBJECT) {
        copy->count = src->count;
        if (src->count > 0) {
            copy->items = xcalloc((size_t)src->count, sizeof(Json *));
            if (src->type == JSON_OBJECT) copy->keys = xcalloc((size_t)src->count, sizeof(char *));
            for (int i = 0; i < src->count; i++) {
                copy->items[i] = json_clone(src->items[i]);
                if (src->type == JSON_OBJECT && src->keys && src->keys[i]) copy->keys[i] = xstrdup(src->keys[i]);
            }
        }
    }
    return copy;
}

void json_free(Json *j) {
    if (!j) return;
    for (int i = 0; i < j->count; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

const Json *json_get(const Json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->count; i++) if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

int json_len(const Json *j) { return j ? j->count : 0; }
const Json *json_at(const Json *j, int i) { return (j && i >= 0 && i < j->count) ? j->items[i] : NULL; }

double json_num(const Json *obj, const char *key, double def) {
    const Json *v = json_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? v->num : def;
}
int json_int(const Json *obj, const char *key, int def) { return (int)json_num(obj, key, def); }
bool json_bool(const Json *obj, const char *key, bool def) {
    const Json *v = json_get(obj, key);
    return (v && v->type == JSON_BOOL) ? v->boolean : def;
}
const char *json_str(const Json *obj, const char *key, const char *def) {
    const Json *v = json_get(obj, key);
    return (v && v->type == JSON_STRING) ? v->str : def;
}
double json_as_num(const Json *v, double def) { return (v && v->type == JSON_NUMBER) ? v->num : def; }
const char *json_as_str(const Json *v, const char *def) { return (v && v->type == JSON_STRING) ? v->str : def; }

/* ----------------------------------------------------------------- writer */

static void jw_put(JsonWriter *w, const char *s, size_t n) {
    if (w->len + n + 1 > w->cap) {
        w->cap = MAX(w->cap * 2, w->len + n + 256);
        w->buf = xrealloc(w->buf, w->cap);
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void jw_sep(JsonWriter *w) {
    if (w->after_key) { w->after_key = false; return; }
    if (w->depth > 0 && w->needs_comma[w->depth - 1]) jw_put(w, ",", 1);
    if (w->depth > 0) {
        jw_put(w, "\n", 1);
        for (int i = 0; i < w->depth; i++) jw_put(w, "  ", 2);
        w->needs_comma[w->depth - 1] = true;
    }
}

void jw_begin_obj(JsonWriter *w) { jw_sep(w); jw_put(w, "{", 1); w->needs_comma[w->depth++] = false; }
void jw_begin_arr(JsonWriter *w) { jw_sep(w); jw_put(w, "[", 1); w->needs_comma[w->depth++] = false; }

static void jw_close(JsonWriter *w, char c) {
    bool had = w->needs_comma[--w->depth];
    if (had) {
        jw_put(w, "\n", 1);
        for (int i = 0; i < w->depth; i++) jw_put(w, "  ", 2);
    }
    jw_put(w, &c, 1);
}
void jw_end_obj(JsonWriter *w) { jw_close(w, '}'); }
void jw_end_arr(JsonWriter *w) { jw_close(w, ']'); }

static void jw_quoted(JsonWriter *w, const char *s) {
    jw_put(w, "\"", 1);
    for (; *s; s++) {
        char esc[8];
        switch (*s) {
        case '"': jw_put(w, "\\\"", 2); break;
        case '\\': jw_put(w, "\\\\", 2); break;
        case '\n': jw_put(w, "\\n", 2); break;
        case '\t': jw_put(w, "\\t", 2); break;
        default:
            if ((u8)*s < 32) { snprintf(esc, sizeof esc, "\\u%04x", (u8)*s); jw_put(w, esc, 6); }
            else jw_put(w, s, 1);
        }
    }
    jw_put(w, "\"", 1);
}

void jw_key(JsonWriter *w, const char *key) {
    jw_sep(w);
    jw_quoted(w, key);
    jw_put(w, ": ", 2);
    w->after_key = true;
}
void jw_str(JsonWriter *w, const char *s) { jw_sep(w); jw_quoted(w, s); }
void jw_num(JsonWriter *w, double v) {
    char b[48];
    jw_sep(w);
    if (v == (double)(i64)v && fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.9g", v);
    jw_put(w, b, strlen(b));
}
void jw_bool(JsonWriter *w, bool v) { jw_sep(w); jw_put(w, v ? "true" : "false", v ? 4 : 5); }
void json_write(JsonWriter *w, const Json *j) {
    if (!j) { jw_put(w, "null", 4); return; }
    switch (j->type) {
    case JSON_NULL:
        jw_put(w, "null", 4);
        break;
    case JSON_BOOL:
        jw_bool(w, j->boolean);
        break;
    case JSON_NUMBER:
        jw_num(w, j->num);
        break;
    case JSON_STRING:
        jw_str(w, j->str ? j->str : "");
        break;
    case JSON_ARRAY:
        jw_begin_arr(w);
        for (int i = 0; i < j->count; i++) { json_write(w, j->items[i]); }
        jw_end_arr(w);
        break;
    case JSON_OBJECT:
        jw_begin_obj(w);
        for (int i = 0; i < j->count; i++) { jw_key(w, j->keys[i]); json_write(w, j->items[i]); }
        jw_end_obj(w);
        break;
    }
}
void jw_free(JsonWriter *w) { free(w->buf); memset(w, 0, sizeof *w); }
