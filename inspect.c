#include "quickjs.h"
#include "cutils.h"
#include "list.h"
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <sys/ioctl.h>

#define COLOR_RED "\x1b[31m"
#define COLOR_GREEN "\x1b[32m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_MARINE "\x1b[36m"
#define COLOR_GRAY "\x1b[1;30m"
#define COLOR_NONE "\x1b[m"

typedef struct {
  int colors : 1;
  int show_hidden : 1;
  int custom_inspect : 1;
  int show_proxy : 1;
  int getters : 1;
  int32_t depth;
  int32_t max_array_length;
  int32_t max_string_length;
  int32_t break_length;
  int32_t compact;
  struct list_head hide_keys;
} inspect_options_t;

typedef struct {
  const char* name;
  JSAtom atom;
  struct list_head link;
} prop_key_t;

static JSValueConst global_object, array_buffer, array_buffer_proto;
static JSAtom inspect_custom_atom;

#define is_control_char(c) ((c) == 8 || (c) == '\f' || (c) == '\n' || (c) == '\r' || (c) == '\t' || (c) == 11)
#define is_alphanumeric_char(c) ((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z')
#define is_digit_char(c) ((c) >= '0' && (c) <= '9')
#define is_newline_char(c) ((c) == '\n')
#define is_identifier_char(c) (is_alphanumeric_char(c) || is_digit_char(c) || (c) == '$' || (c) == '_')

static inline size_t
min_size(size_t a, size_t b) {
  if(a < b)
    return a;
  else
    return b;
}

static inline char
escape_char_letter(char c) {
  switch(c) {
    case '\t': return 't';
    case '\r': return 'r';
    case '\n': return 'n';
    case '\\': return '\\';
    case '\'': return '\'';
  }
  return 0;
}

static inline int
is_escape_char(char c) {
  return is_control_char(c) || c == 0x5c || c == 0x27;
}

static int
is_identifier(const char* str) {
  if(!((*str >= 'A' && *str <= 'Z') || (*str >= 'a' && *str <= 'z') || *str == '$'))
    return 0;
  while(*++str) {
    if(!is_identifier_char(*str))
      return 0;
  }
  return 1;
}

static int
is_integer(const char* str) {
  if(!(*str >= '1' && *str <= '9') && !(*str == '0' && str[1] == '\0'))
    return 0;
  while(*++str) {
    if(!is_digit_char(*str))
      return 0;
  }
  return 1;
}

static size_t
predicate_find(const char* str, size_t len, int (*pred)(char)) {
  size_t pos;
  for(pos = 0; pos < len; pos++)
    if(pred(str[pos]))
      break;
  return pos;
}

static size_t
byte_chr(const char* str, size_t len, char c) {
  const char* s = str;
  const char* t = s + len;

  for(;;) {
    if(s == t)
      break;
    if(*s == c)
      break;
    ++s;
  }
  return s - str;
}

static size_t
ansi_skip(const char* str, size_t len) {
  size_t pos = 0;
  if(str[pos] == 0x1b) {
    if(++pos < len && str[pos] == '[') {
      while(++pos < len)
        if(is_alphanumeric_char(str[pos]))
          break;
      if(++pos < len && str[pos] == '~')
        ++pos;
      return pos;
    }
  }
  return 0;
}

static size_t
ansi_length(const char* str, size_t len) {
  size_t i, n = 0, p;
  for(i = 0; i < len;) {
    if((p = ansi_skip(&str[i], len - i)) > 0) {
      i += p;
      continue;
    }
    n++;
    i++;
  }
  return n;
}

static size_t
ansi_truncate(const char* str, size_t len, size_t limit) {
  size_t i, n = 0, p;
  for(i = 0; i < len;) {
    if((p = ansi_skip(&str[i], len - i)) > 0) {
      i += p;
      continue;
    }
    n += is_escape_char(str[i]) ? 2 : 1;
    if(n > limit)
      break;
    i++;
  }
  return i;
}

static void
dbuf_put_escaped(DynBuf* db, const char* str, size_t len) {
  size_t i = 0, j;
  while(i < len) {
    if((j = predicate_find(&str[i], len - i, is_escape_char))) {
      dbuf_put(db, (const uint8_t*)&str[i], j);
      i += j;
    }
    if(i == len)
      break;
    dbuf_putc(db, '\\');
    dbuf_putc(db, escape_char_letter(str[i]));
    i++;
  }
}

static const char*
dbuf_last_line(DynBuf* db, size_t* len) {
  size_t i;
  for(i = db->size; i > 0; i--)
    if(db->buf[i - 1] == '\n')
      break;
  if(len)
    *len = db->size - i;
  return (const char*)&db->buf[i];
}

static int32_t
dbuf_get_column(DynBuf* db) {
  size_t len;
  const char* str;
  if(db->size) {
    str = dbuf_last_line(db, &len);
    return ansi_length(str, len);
  }
  return 0;
}

static inline void
dbuf_put_colorstr(DynBuf* db, const char* str, const char* color, int with_color) {

  if(with_color)
    dbuf_putstr(db, color);
  dbuf_putstr(db, str);
  if(with_color)
    dbuf_putstr(db, COLOR_NONE);
}

#define js_object_tmpmark_set(value)                                                                                   \
  do { ((uint8_t*)JS_VALUE_GET_OBJ((value)))[5] |= 0x40; } while(0);
#define js_object_tmpmark_clear(value)                                                                                 \
  do { ((uint8_t*)JS_VALUE_GET_OBJ((value)))[5] &= ~0x40; } while(0);
#define js_object_tmpmark_isset(value) (((uint8_t*)JS_VALUE_GET_OBJ((value)))[5] & 0x40)

static inline JSValue
js_new_number(JSContext* ctx, int32_t n) {
  if(n == INT32_MAX)
    return JS_NewFloat64(ctx, INFINITY);
  return JS_NewInt32(ctx, n);
}

static inline JSValue
js_new_bool_or_number(JSContext* ctx, int32_t n) {
  if(n == 0)
    return JS_NewBool(ctx, FALSE);
  return js_new_number(ctx, n);
}

static inline JSValue
js_symbol_constructor_get(JSContext* ctx) {
  static JSValue ctor;

  if((JS_VALUE_GET_TAG(ctor) | JS_VALUE_GET_INT(ctor)) == 0)
    ctor = JS_GetPropertyStr(ctx, global_object, "Symbol");

  return ctor;
}

static JSValue
js_symbol_invoke_static(JSContext* ctx, const char* name, JSValueConst arg) {
  JSValue ret;
  JSAtom method_name = JS_NewAtom(ctx, name);
  ret = JS_Invoke(ctx, js_symbol_constructor_get(ctx), method_name, 1, &arg);
  JS_FreeAtom(ctx, method_name);
  return ret;
}

static inline JSValue
js_symbol_to_string(JSContext* ctx, JSValueConst sym) {
  JSValue value;
  value = js_symbol_invoke_static(ctx, "keyFor", sym);
  if(!JS_IsUndefined(value))
    return value;
  return JS_AtomToString(ctx, JS_ValueToAtom(ctx, sym));
}

static inline const char*
js_symbol_to_c_string(JSContext* ctx, JSValueConst sym) {
  JSValue value = js_symbol_to_string(ctx, sym);
  return JS_ToCString(ctx, value);
}

static void
js_inspect_options_init(inspect_options_t* opts) {
  opts->colors = TRUE;
  opts->show_hidden = FALSE;
  opts->custom_inspect = TRUE;
  opts->show_proxy = FALSE;
  opts->getters = FALSE;
  opts->depth = INT32_MAX;
  opts->max_array_length = 100;
  opts->max_string_length = INT32_MAX;
  opts->break_length = 80;
  opts->compact = 5;

  init_list_head(&opts->hide_keys);
}

static void
js_inspect_options_get(JSContext* ctx, JSValueConst object, inspect_options_t* opts) {
  JSValue value;
  if(!JS_IsUndefined((value = JS_GetPropertyStr(ctx, object, "colors")))) {
    opts->colors = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
  }

  if(!JS_IsUndefined((value = JS_GetPropertyStr(ctx, object, "showHidden")))) {
    opts->show_hidden = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "customInspect");
  opts->custom_inspect = JS_ToBool(ctx, value);
  JS_FreeValue(ctx, value);

  if(!JS_IsUndefined((value = JS_GetPropertyStr(ctx, object, "showProxy")))) {
    opts->show_proxy = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
  }

  if(!JS_IsUndefined((value = JS_GetPropertyStr(ctx, object, "getters")))) {
    opts->getters = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "depth");
  if(!JS_IsUndefined(value)) {
    if(isinf(JS_VALUE_GET_FLOAT64(value)))
      opts->depth = INT32_MAX;
    else
      JS_ToInt32(ctx, &opts->depth, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "maxArrayLength");
  if(!JS_IsUndefined(value)) {
    if(isinf(JS_VALUE_GET_FLOAT64(value)))
      opts->max_array_length = INT32_MAX;
    else
      JS_ToInt32(ctx, &opts->max_array_length, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "maxStringLength");
  if(!JS_IsUndefined(value)) {
    if(isinf(JS_VALUE_GET_FLOAT64(value)))
      opts->max_string_length = INT32_MAX;
    else
      JS_ToInt32(ctx, &opts->max_string_length, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "breakLength");
  if(!JS_IsUndefined(value)) {
    if(isinf(JS_VALUE_GET_FLOAT64(value)))
      opts->break_length = INT32_MAX;
    else
      JS_ToInt32(ctx, &opts->break_length, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "compact");
  if(!JS_IsUndefined(value)) {
    if(JS_VALUE_GET_TAG(value) == JS_TAG_BOOL && JS_VALUE_GET_BOOL(value) == 0)
      opts->compact = INT32_MAX;
    else if(JS_VALUE_GET_TAG(value) == JS_TAG_FLOAT64 && isinf(JS_VALUE_GET_FLOAT64(value)))
      opts->compact = INT32_MAX;
    else
      JS_ToInt32(ctx, &opts->compact, value);
    JS_FreeValue(ctx, value);
  }

  value = JS_GetPropertyStr(ctx, object, "hideKeys");
  if(JS_IsArray(ctx, value)) {
    uint32_t len, pos;
    JSValue lval = JS_GetPropertyStr(ctx, value, "length");
    JS_ToUint32(ctx, &len, lval);
    JS_FreeValue(ctx, lval);
    for(pos = 0; pos < len; pos++) {
      JSValue item = JS_GetPropertyUint32(ctx, value, pos);
      prop_key_t* key = js_mallocz(ctx, sizeof(prop_key_t));
      key->name = JS_ToCString(ctx, item);
      key->atom = JS_ValueToAtom(ctx, item);
      list_add(&key->link, &opts->hide_keys);
      JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, value);
  }
}

static JSValue
js_inspect_options_object(JSContext* ctx, inspect_options_t* opts) {
  JSValue arr, ret = JS_NewObject(ctx);
  uint32_t n;
  struct list_head* el;

  JS_SetPropertyStr(ctx, ret, "colors", JS_NewBool(ctx, opts->colors));
  JS_SetPropertyStr(ctx, ret, "showHidden", JS_NewBool(ctx, opts->show_hidden));
  JS_SetPropertyStr(ctx, ret, "customInspect", JS_NewBool(ctx, opts->custom_inspect));
  JS_SetPropertyStr(ctx, ret, "showProxy", JS_NewBool(ctx, opts->show_proxy));
  JS_SetPropertyStr(ctx, ret, "getters", JS_NewBool(ctx, opts->getters));

  JS_SetPropertyStr(ctx, ret, "depth", js_new_number(ctx, opts->depth));
  JS_SetPropertyStr(ctx, ret, "maxArrayLength", js_new_number(ctx, opts->max_array_length));
  JS_SetPropertyStr(ctx, ret, "maxStringLength", js_new_number(ctx, opts->max_string_length));
  JS_SetPropertyStr(ctx, ret, "breakLength", js_new_number(ctx, opts->break_length));
  JS_SetPropertyStr(ctx, ret, "compact", js_new_bool_or_number(ctx, opts->compact));

  arr = JS_NewArray(ctx);
  n = 0;
  list_for_each(el, &opts->hide_keys) {
    prop_key_t* key = list_entry(el, prop_key_t, link);
    JS_SetPropertyUint32(ctx, arr, n++, JS_AtomToValue(ctx, key->atom));
  }
  JS_SetPropertyStr(ctx, ret, "hideKeys", arr);

  return ret;
}

static int
js_inspect_hidden_key(inspect_options_t* opts, JSAtom atom) {
  struct list_head* el;
  list_for_each(el, &opts->hide_keys) {
    prop_key_t* key = list_entry(el, prop_key_t, link);
    if(key->atom == atom) //! strcmp(key->name, str))
      return 1;
  }
  return 0;
}

static JSAtom
js_inspect_custom_atom(JSContext* ctx) {
  static JSValue sym;

  if((JS_VALUE_GET_TAG(sym) | JS_VALUE_GET_INT(sym)) == 0) {
    JSValue key = JS_NewString(ctx, "nodejs.util.inspect.custom");
    sym = js_symbol_invoke_static(ctx, "for", key);
    JS_FreeValue(ctx, key);
    inspect_custom_atom = JS_ValueToAtom(ctx, sym);
    JS_FreeValue(ctx, sym);
  }
  return inspect_custom_atom;
}

static const char*
js_inspect_custom_call(JSContext* ctx, JSValueConst obj, inspect_options_t* opts, int32_t depth) {
  JSValue inspect;
  const char* str = 0;
  JSAtom prop = js_inspect_custom_atom(ctx);

  if(JS_HasProperty(ctx, obj, prop))
    inspect = JS_GetProperty(ctx, obj, prop);
  else
    inspect = JS_GetPropertyStr(ctx, obj, "inspect");

  if(JS_IsFunction(ctx, inspect)) {
    JSValueConst args[2];
    JSValue ret;

    args[0] = js_new_number(ctx, depth);
    args[1] = js_inspect_options_object(ctx, opts);

    ret = JS_Call(ctx, inspect, obj, 2, args);

    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);

    str = JS_ToCString(ctx, ret);
    JS_FreeValue(ctx, ret);
  }
  JS_FreeValue(ctx, inspect);

  return str;
}

static void
js_inspect_newline(DynBuf* buf, int32_t depth) {
  dbuf_putc(buf, '\n');

  while(depth-- > 0) dbuf_putstr(buf, "  ");
}
char*
strndup(const char* s, size_t n) {
  char* r = malloc(n + 1);
  if(r == NULL)
    return NULL;
  memcpy(r, s, n);
  r[n] = '\0';
  return r;
}

static char*
js_class_name(JSContext* ctx, JSValueConst value) {
  JSValue proto, ctor;
  const char* str;
  char* name = 0;
  int namelen;
  proto = JS_GetPrototype(ctx, value);
  ctor = JS_GetPropertyStr(ctx, proto, "constructor");
  if((str = JS_ToCString(ctx, ctor))) {
    if(!strncmp(str, "function ", 9)) {
      namelen = byte_chr(str + 9, strlen(str) - 9, '(');
      name = js_strndup(ctx, str + 9, namelen);
    }
  }
  if(!name) {
    if(str)
      JS_FreeCString(ctx, str);
    if((str = JS_ToCString(ctx, JS_GetPropertyStr(ctx, ctor, "name"))))
      name = js_strdup(ctx, str);
  }
  if(str)
    JS_FreeCString(ctx, str);
  return name;
}

static int
js_is_arraybuffer(JSContext* ctx, JSValueConst value) {
  int ret = 0;
  int n, m;
  void* obj = JS_VALUE_GET_OBJ(value);
  char* name = 0;

  if((name = js_class_name(ctx, value))) {
    n = strlen(name);
    m = n >= 11 ? n - 11 : 0;
    // printf("  name=%s\n", name + m);
    if(!strcmp(name + m, "ArrayBuffer"))
      ret = 1;
  }
  if(!ret) {
    const char* str;
    if(JS_IsInstanceOf(ctx, value, array_buffer))
      ret = 1;
    else if(!JS_IsArray(ctx, value) && (str = JS_ToCString(ctx, value))) {
      ret = strstr(str, "ArrayBuffer]") != 0;
      //   ret = !strcmp(str, "[object ArrayBuffer]");
      JS_FreeCString(ctx, str);
    }
  }
  if(name)
    js_free(ctx, (void*)name); // printf("js_is_arraybuffer ret=%i\n", ret);
  return ret;
}

static int
js_inspect_screen_width(void) {
  struct winsize w = {.ws_col = -1, .ws_row = -1};
  ioctl(1, TIOCGWINSZ, &w);
  return w.ws_col;
}

static int
js_inspect_arraybuffer(JSContext* ctx, DynBuf* buf, JSValueConst value, inspect_options_t* opts, int32_t depth) {
  const char *str, *str2;
  uint8_t* ptr;
  size_t i, size;
  int break_len = js_inspect_screen_width();
  int column = dbuf_get_column(buf);

  if(break_len > opts->break_length)
    break_len = opts->break_length;

  ptr = JS_GetArrayBuffer(ctx, &size, value);

  //  printf("maxArrayLength: %i\n", opts->max_array_length);

  str = JS_ToCString(ctx, value);
  str2 = strstr(str, "ArrayBuffer");

  while(str2 > str && !isspace(*--str2))
    ;

  size_t slen = byte_chr(str2, strlen(str2), ']');

  dbuf_put(buf, str2, slen);
  dbuf_printf(buf, " { byteLength: %zu [", size);
  for(i = 0; i < size; i++) {
    if(i == opts->max_array_length)
      break;

    if(column == break_len) {
      js_inspect_newline(buf, (opts->depth - depth) + 1);
      column = 0;
    } else
      column += 3;

    dbuf_printf(buf, " %02x", ptr[i]);
  }
  dbuf_printf(buf, "... %zu more bytes", size - i);
  dbuf_putstr(buf, " ] }");

  return 0;
}

static int
js_inspect_print(JSContext* ctx, DynBuf* buf, JSValueConst value, inspect_options_t* opts, int32_t depth) {

  int tag = JS_VALUE_GET_TAG(value);
  int32_t level = opts->depth - depth;
  int compact = level >= (opts->depth - opts->compact);

  switch(tag) {
    case JS_TAG_FLOAT64:
    case JS_TAG_BIG_DECIMAL:
    case JS_TAG_BIG_INT:
    case JS_TAG_BIG_FLOAT: {
      const char* str;
      size_t len;
      str = JS_ToCStringLen(ctx, &len, value);
      if(tag != JS_TAG_SYMBOL && opts->colors)
        dbuf_putstr(buf, COLOR_YELLOW);
      dbuf_put(buf, (const uint8_t*)str, len);
      JS_FreeCString(ctx, str);
      if(tag <= JS_TAG_BIG_FLOAT)
        dbuf_putc(buf, tag == JS_TAG_BIG_DECIMAL ? 'm' : tag == JS_TAG_BIG_FLOAT ? 'l' : 'n');
      if(opts->colors)
        dbuf_putstr(buf, COLOR_NONE);
      break;
    }

    case JS_TAG_SYMBOL: {
      value = js_symbol_to_string(ctx, value);
      if(opts->colors)
        dbuf_putstr(buf, COLOR_GREEN);
      dbuf_putstr(buf, "Symbol");
      __attribute__((fallthrough));
    }

    case JS_TAG_STRING: {
      const char* str;
      size_t pos, len, max_len, limit;
      str = JS_ToCStringLen(ctx, &len, value);
      max_len = min_size(opts->break_length - dbuf_get_column(buf) - 4, len);

      if(tag != JS_TAG_SYMBOL && opts->colors)
        dbuf_putstr(buf, COLOR_GREEN);
      dbuf_putc(buf, tag == JS_TAG_SYMBOL ? '(' : '\'');
      limit = min_size(opts->max_string_length, len);

      for(pos = 0; pos < limit;) {
        size_t n, eol;

        n = limit - pos;
        if(!compact) {
          eol = byte_chr(&str[pos], n, '\n');
          if(str[pos + eol] == '\n')
            eol++;
          n = ansi_truncate(&str[pos], eol, max_len);
        }

        if(pos > 0) {
          dbuf_putstr(buf, opts->colors ? "'" COLOR_NONE " +" : "' +");
          js_inspect_newline(buf, level + 1);
          dbuf_putstr(buf, opts->colors ? COLOR_GREEN "'" : "'");
        } else {
          max_len = opts->break_length - (level * 2) - 4;
        }

        dbuf_put_escaped(buf, &str[pos], n);
        pos += n;
      }
      JS_FreeCString(ctx, str);
      dbuf_putc(buf, tag == JS_TAG_SYMBOL ? ')' : '\'');

      if(opts->colors)
        dbuf_putstr(buf, COLOR_NONE);

      if(limit < len) {
        if(dbuf_get_column(buf) + 26 > opts->break_length)
          js_inspect_newline(buf, level + 1);
        dbuf_printf(buf, "... %zu more characters", len - pos);
      }
      break;
    }

    case JS_TAG_OBJECT: {
      int is_array = JS_IsArray(ctx, value);
      uint32_t nprops, pos, len, limit;
      JSPropertyEnum* props;
      const char* s;

      if(JS_IsInstanceOf(ctx, value, array_buffer) || js_is_arraybuffer(ctx, value))
        return js_inspect_arraybuffer(ctx, buf, value, opts, depth);

      if(js_object_tmpmark_isset(value)) {
        JS_ThrowTypeError(ctx, "circular reference");
        return -1;
      }

      if(opts->custom_inspect && (s = js_inspect_custom_call(ctx, value, opts, depth))) {
        dbuf_putstr(buf, s);
        return 0;
      }

      if(JS_GetOwnPropertyNames(ctx,
                                &props,
                                &nprops,
                                value,
                                JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | (opts->show_hidden ? 0 : JS_GPN_ENUM_ONLY)))
        return -1;

      if(JS_IsFunction(ctx, value)) {
        JSValue name;
        dbuf_putstr(buf, opts->colors ? COLOR_MARINE "[Function" : "[Function");
        name = JS_GetPropertyStr(ctx, value, "name");
        if(!JS_IsUndefined(name)) {
          s = JS_ToCString(ctx, name);
          if(*s) {
            dbuf_putstr(buf, ": ");
            dbuf_putstr(buf, s);
          }
          JS_FreeCString(ctx, s);
        }
        dbuf_putstr(buf, opts->colors ? "]" COLOR_NONE : "]");
        if(nprops)
          dbuf_putc(buf, ' ');
        else
          return 0;
      }
      if(depth < 0) {
        dbuf_put_colorstr(buf, is_array ? "[Array]" : "[Object]", COLOR_MARINE, opts->colors);
        return 0;
      }

      js_object_tmpmark_set(value);

      if(is_array) {
        JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, value, "length"));
        dbuf_putstr(buf, compact ? "[ " : "[");
        if(!compact)
          js_inspect_newline(buf, level + 1);

        limit = min_size(opts->max_array_length, len);

        for(pos = 0; pos < len; pos++) {
          if(pos > 0) {
            dbuf_putstr(buf, compact ? ", " : ",");
            if(!compact)
              js_inspect_newline(buf, level + 1);
          }
          if(pos == limit)
            break;
          js_inspect_print(ctx, buf, JS_GetPropertyUint32(ctx, value, pos), opts, depth - 1);
        }
        if(limit < len) {
          if(dbuf_get_column(buf) + 20 > opts->break_length)
            js_inspect_newline(buf, level + 1);
          dbuf_printf(buf, "... %u more item", len - pos);
          if(pos + 1 < len)
            dbuf_putc(buf, 's');
        }
        /*if(nprops <= len) {
          if(!compact)
            js_inspect_newline(buf, level);
          dbuf_putstr(buf, compact ? " ]" : "]");
          return 0;
        }*/
      }

      if(!is_array) {
        dbuf_putstr(buf, compact ? "{ " : "{");
        len = 0;
      }

      for(pos = 0; pos < nprops; pos++) {
        JSPropertyDescriptor desc;
        const char* name;
        JSValue key = JS_AtomToValue(ctx, props[pos].atom);

        name = JS_AtomToCString(ctx, props[pos].atom);

        if(!JS_IsSymbol(key)) {
          if((is_array && is_integer(name)) || js_inspect_hidden_key(opts, props[pos].atom)) {
            JS_FreeValue(ctx, key);
            JS_FreeCString(ctx, name);
            continue;
          }
        }

        if(pos > 0)
          dbuf_putstr(buf, compact ? ", " : ",");

        if(!compact)
          js_inspect_newline(buf, level + 1);

        if(!JS_IsSymbol(key) && is_identifier(name)) {
          dbuf_putstr(buf, name);
        } else {
          dbuf_putc(buf, '[');
          js_inspect_print(ctx, buf, key, opts, depth - 1);
          dbuf_putc(buf, ']');
        }
        dbuf_putstr(buf, ": ");
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, key);

        JS_GetOwnProperty(ctx, &desc, value, props[pos].atom);

        if(desc.flags & JS_PROP_GETSET)
          dbuf_put_colorstr(buf,
                            JS_IsUndefined(desc.getter) ? "[Setter]"
                                                        : JS_IsUndefined(desc.setter) ? "[Getter]" : "[Getter/Setter]",
                            COLOR_MARINE,
                            opts->colors);
        else
          js_inspect_print(ctx, buf, desc.value, opts, depth - 1);
        len++;
      }
      js_object_tmpmark_clear(value);

      if(!compact && len)
        js_inspect_newline(buf, level);
      dbuf_putstr(buf, is_array ? (compact ? " ]" : "]") : (compact ? " }" : "}"));
      break;
    }

    case JS_TAG_INT: {
      int i = JS_VALUE_GET_INT(value);
      if(opts->colors)
        dbuf_putstr(buf, COLOR_YELLOW);
      dbuf_printf(buf, "%i", i);
      if(opts->colors)
        dbuf_putstr(buf, COLOR_NONE);
      break;
    }

    case JS_TAG_BOOL: {
      if(opts->colors)
        dbuf_putstr(buf, COLOR_YELLOW);
      dbuf_putstr(buf, JS_VALUE_GET_BOOL(value) ? "true" : "false");
      if(opts->colors)
        dbuf_putstr(buf, COLOR_NONE);
      break;
    }

    case JS_TAG_NULL: {
      dbuf_putstr(buf, "null");
      break;
    }

    case JS_TAG_UNDEFINED: {
      dbuf_put_colorstr(buf, "undefined", COLOR_GRAY, opts->colors);
      break;
    }

    case JS_TAG_EXCEPTION: {
      dbuf_put_colorstr(buf, "[exception]", COLOR_RED, opts->colors);
      break;
    }

      /*  case JS_TAG_FLOAT64: {
          double d = JS_VALUE_GET_FLOAT64(value);
          if(opts->colors)
            dbuf_putstr(buf, COLOR_YELLOW);
          if(d == -INFINITY)
            dbuf_putstr(buf, "-Infinity");
          else if(d == INFINITY)
            dbuf_putstr(buf, "Infinity");
          else if(JS_VALUE_IS_NAN(value))
            dbuf_putstr(buf, "NaN");
          else
            dbuf_printf(buf, "%lg", d);
          if(opts->colors)
            dbuf_putstr(buf, COLOR_NONE);
          break;
        }*/

    default: {
      return -1;
    }
  }
  return 0;
}

static JSValue
js_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  DynBuf dbuf;
  inspect_options_t options;
  int32_t depth;
  int optsind = 1;
  JSValue ret;

  dbuf_init(&dbuf);

  js_inspect_options_init(&options);

  if(argc > 1 && JS_IsNumber(argv[1]))
    optsind++;

  if(optsind < argc)
    js_inspect_options_get(ctx, argv[optsind], &options);

  if(optsind > 1)
    JS_ToInt32(ctx, &depth, argv[1]);
  else
    depth = options.depth;

  js_inspect_print(ctx, &dbuf, argv[0], &options, depth);

  ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);

  dbuf_free(&dbuf);

  return ret;
}

static const JSCFunctionListEntry js_inspect_funcs[] = {JS_CFUNC_DEF("inspect", 1, js_inspect)};

static int
js_inspect_init(JSContext* ctx, JSModuleDef* m) {
  JSValue inspect;

  JS_SetModuleExportList(ctx, m, js_inspect_funcs, countof(js_inspect_funcs));

  inspect = JS_NewCFunction(ctx, js_inspect, "inspect", 2);
  JS_SetModuleExport(ctx, m, "default", inspect);

  global_object = JS_GetGlobalObject(ctx);
  array_buffer = JS_GetPropertyStr(ctx, global_object, "ArrayBuffer");

  if(!JS_IsConstructor(ctx, array_buffer))
    JS_ThrowTypeError(ctx, "ArrayBuffer is not a constructor");

  array_buffer_proto = JS_GetPropertyStr(ctx, array_buffer, "prototype");
  return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_inspect
#endif

JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;
  m = JS_NewCModule(ctx, module_name, js_inspect_init);
  if(!m)
    return NULL;
  JS_AddModuleExportList(ctx, m, js_inspect_funcs, countof(js_inspect_funcs));
  JS_AddModuleExport(ctx, m, "default");
  return m;
}
