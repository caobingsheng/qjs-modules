#include "include/defines.h"
#include <cutils.h>
#include <quickjs.h>
#include "include/buffer-utils.h"
#include "include/char-utils.h"
#include "include/debug.h"

#include <limits.h>
#include <string.h>

#include "include/path.h"
#include "include/utils.h"
#ifdef _WIN32
#include <windows.h>
#endif

/**
 * \defgroup quickjs-path QuickJS module: path - Directory path
 * @{
 */
thread_local JSValue path_object = {{JS_TAG_UNDEFINED}};

enum path_methods {
  METHOD_ABSOLUTE = 0,
  METHOD_APPEND,
  METHOD_BASENAME,
  METHOD_CANONICAL,
  METHOD_COLLAPSE,
  METHOD_CONCAT,
  METHOD_DIRNAME,
  METHOD_EXISTS,
  METHOD_EXTNAME,
  METHOD_FIND,
  METHOD_FNMATCH,
  METHOD_GETCWD,
  METHOD_GETHOME,
  METHOD_GETSEP,
  METHOD_IS_ABSOLUTE,
  METHOD_IS_RELATIVE,
  METHOD_IS_DIRECTORY,
  METHOD_IS_FILE,
  METHOD_IS_CHARDEV,
  METHOD_IS_BLOCKDEV,
  METHOD_IS_FIFO,
  METHOD_IS_SOCKET,
  METHOD_IS_SYMLINK,
  METHOD_IS_SEPARATOR,
  METHOD_LENGTH,
  METHOD_LEN_S,
  METHOD_NORMALIZE,
  METHOD_COMPONENTS,
  METHOD_READLINK,
  METHOD_REALPATH,
  METHOD_RELATIVE,
  METHOD_RIGHT,
  METHOD_SKIP,
  METHOD_SKIPS,
  METHOD_SKIP_SEPARATOR,
  METHOD_SPLIT,
  METHOD_AT
};

static JSValue
js_path_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  const char *a = 0, *b = 0;
  char buf[PATH_MAX + 1];
  size_t alen = 0, blen = 0, pos;
  JSValue ret = JS_UNDEFINED;
  if(argc > 0) {
    a = JS_ToCStringLen(ctx, &alen, argv[0]);

    if(argc > 1)
      b = JS_ToCStringLen(ctx, &blen, argv[1]);
  }

  switch(magic) {
    case METHOD_BASENAME: {
      const char* o = basename(a);
      size_t len = strlen(o);
      if(b && str_ends(o, b))
        len -= strlen(b);
      ret = JS_NewStringLen(ctx, o, len);
      break;
    }

    case METHOD_DIRNAME: {
      if((pos = str_rchrs(a, "/\\", 2)) < alen)
        ret = JS_NewStringLen(ctx, a, pos);
      else if(alen == 1 && a[0] == '.')
        ret = JS_NULL;
      else
        ret = JS_NewStringLen(ctx, ".", 1);
      break;
    }

    case METHOD_READLINK: {
      ssize_t r;
      memset(buf, 0, sizeof(buf));
      if((r = readlink(a, buf, sizeof(buf)) > 0)) {
        ret = JS_NewString(ctx, buf);
      }
      break;
    }

#ifndef __wasi__
    case METHOD_REALPATH: {
#ifdef _WIN32
      char dst[PATH_MAX + 1];
      size_t len = GetFullPathNameA(buf, PATH_MAX + 1, dst, NULL);
      ret = JS_NewStringLen(ctx, dst, len);
#else
      if(realpath(a, buf))
        ret = JS_NewString(ctx, buf);
#endif
      break;
    }
#endif
    case METHOD_EXISTS: {
      ret = JS_NewBool(ctx, path_exists(a));
      break;
    }

    case METHOD_EXTNAME: {
      ret = JS_NewString(ctx, path_extname(a));
      break;
    }

    case METHOD_GETCWD: {
      if(getcwd(buf, sizeof(buf)))
        ret = JS_NewString(ctx, buf);
      break;
    }

    case METHOD_IS_ABSOLUTE: {
      if(a && a[0])
        ret = JS_NewBool(ctx, path_isabs(a));
      break;
    }

    case METHOD_IS_RELATIVE: {
      if(a && a[0])
        ret = JS_NewBool(ctx, !path_isabs(a));
      break;
    }

    case METHOD_IS_DIRECTORY: {
      ret = JS_NewBool(ctx, path_is_directory(a));
      break;
    }

    case METHOD_IS_FILE: {
      ret = JS_NewBool(ctx, path_is_file(a));
      break;
    }

    case METHOD_IS_CHARDEV: {
      ret = JS_NewBool(ctx, path_is_chardev(a));
      break;
    }

    case METHOD_IS_BLOCKDEV: {
      ret = JS_NewBool(ctx, path_is_blockdev(a));
      break;
    }

    case METHOD_IS_FIFO: {
      ret = JS_NewBool(ctx, path_is_fifo(a));
      break;
    }

    case METHOD_IS_SOCKET: {
      ret = JS_NewBool(ctx, path_is_socket(a));
      break;
    }

    case METHOD_IS_SYMLINK: {
      ret = JS_NewBool(ctx, path_is_symlink(a));
      break;
    }

    case METHOD_IS_SEPARATOR: {
      if(a && a[0])
        ret = JS_NewBool(ctx, path_issep(a[0]));
      break;
    }

    case METHOD_COLLAPSE: {
      char* s = malloc(alen + 1);

      memcpy(s, a, alen);
      s[alen] = '\0';
      size_t newlen;

      newlen = path_collapse(s, alen);
      ret = JS_NewStringLen(ctx, s, newlen);
      free(s);
      break;
    }

    case METHOD_FNMATCH: {
      int32_t flags = 0;
      if(argc > 2)
        JS_ToInt32(ctx, &flags, argv[2]);
      ret = JS_NewInt32(ctx, path_fnmatch(a, alen, b, blen, flags));
      break;
    }

#ifndef __wasi__
    case METHOD_GETHOME: {
#ifdef _WIN32
      ret = JS_NewString(ctx, getenv("USERPROFILE"));
#else
      ret = JS_NewString(ctx, path_gethome(getuid()));
#endif
      break;
    }
#endif

    case METHOD_GETSEP: {
      char c;
      if((c = path_getsep(a)) != '\0')
        ret = JS_NewStringLen(ctx, &c, 1);
      break;
    }

    case METHOD_LENGTH: {
      ret = JS_NewUint32(ctx, path_num_components(a));
      break;
    }

    case METHOD_COMPONENTS: {
      uint32_t n = UINT32_MAX;
      if(argc > 1)
        JS_ToUint32(ctx, &n, argv[1]);
      ret = JS_NewUint32(ctx, path_components(a, alen, n));
      break;
    }

    case METHOD_RIGHT: {
      ret = JS_NewUint32(ctx, path_right(a, alen));
      break;
    }

    case METHOD_SKIP: {
      ret = JS_NewUint32(ctx, path_skip(a, alen));
      break;
    }

    case METHOD_SKIP_SEPARATOR: {
      int64_t n = 0;
      if(argc > 1)
        JS_ToInt64(ctx, &n, argv[1]);
      ret = JS_NewUint32(ctx, path_skip_separator(a, alen, n));
      break;
    }
    case METHOD_AT: {
      uint64_t idx;
      size_t len;
      const char* p;

      JS_ToIndex(ctx, &idx, argv[1]);
      p = path_at(a, &len, idx);
      ret = JS_NewStringLen(ctx, p, len);
      break;
    }
  }

  if(a)
    js_cstring_free(ctx, a);
  if(b)
    js_cstring_free(ctx, b);

  return ret;
}

static JSValue
js_path_method_dbuf(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  const char *a = 0, *b = 0;
  DynBuf db;
  size_t alen = 0, blen = 0;

  if(argc > 0) {
    if(!JS_IsString(argv[0]))
      return JS_ThrowTypeError(ctx, "argument 1 must be a string");

    a = JS_ToCStringLen(ctx, &alen, argv[0]);
    if(argc > 1)
      b = JS_ToCStringLen(ctx, &blen, argv[1]);
  }

  js_dbuf_init(ctx, &db);

  switch(magic) {
    case METHOD_ABSOLUTE: {
      path_absolute(a, &db);
      break;
    }

    case METHOD_APPEND: {
      path_append(a, alen, &db);
      break;
    }

    case METHOD_CANONICAL: {
      path_canonical(a, &db);
      break;
    }

    case METHOD_CONCAT: {
      path_concat(a, alen, b, blen, &db);
      break;
    }

    case METHOD_FIND: {
      path_find(a, b, &db);
      break;
    }

    case METHOD_RELATIVE: {
      DynBuf cwd = {0, 0, 0, 0, 0, 0};

      if(b == NULL) {
        dbuf_init2(&cwd, JS_GetRuntime(ctx), (DynBufReallocFunc*)js_realloc_rt);
        b = path_getcwd(&cwd);
      }
      path_relative(a, b, &db);
      if(b == (const char*)cwd.buf) {
        dbuf_free(&db);
        b = NULL;
      }
      break;
    }

    case METHOD_NORMALIZE: {
      BOOL symbolic = FALSE;
      if(argc > 1)
        symbolic = JS_ToBool(ctx, argv[1]);
      path_normalize(a, &db, symbolic);
      break;
    }
  }

  if(a)
    js_cstring_free(ctx, a);
  if(b)
    js_cstring_free(ctx, b);

  return dbuf_tostring_free(&db, ctx);
}

static JSValue
js_path_join(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  const char* str;
  DynBuf db;
  int i;
  size_t len = 0;
  JSValue ret = JS_UNDEFINED;
  js_dbuf_init(ctx, &db);
  js_dbuf_init(ctx, &db);
  for(i = 0; i < argc; i++) {
    str = JS_ToCStringLen(ctx, &len, argv[i]);
    path_append(str, len, &db);
    js_cstring_free(ctx, str);
  }
  if(db.size) {
    ret = JS_NewStringLen(ctx, (const char*)db.buf, db.size);
  }
  dbuf_free(&db);
  return ret;
}

static JSValue
js_path_parse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  const char *str, *base, *ext;
  size_t len = 0, rootlen, dirlen;
  JSValue ret = JS_UNDEFINED;

  str = JS_ToCStringLen(ctx, &len, argv[0]);

  base = basename(str);
  dirlen = base - str - 1;
  rootlen = path_root(str, len);
  ext = path_extname(str);

  ret = JS_NewObject(ctx);

  js_set_propertystr_stringlen(ctx, ret, "root", str, rootlen);
  js_set_propertystr_stringlen(ctx, ret, "dir", str, dirlen);
  js_set_propertystr_stringlen(ctx, ret, "base", base, strlen(base));
  js_set_propertystr_stringlen(ctx, ret, "ext", ext, strlen(ext));
  js_set_propertystr_stringlen(ctx, ret, "name", base, strlen(base) - strlen(ext));

  js_cstring_free(ctx, str);

  return ret;
}

static JSValue
js_path_format(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValueConst obj = argv[0];
  const char *dir, *root, *base, *name, *ext;
  JSValue ret = JS_UNDEFINED;
  DynBuf db;

  js_dbuf_init(ctx, &db);

  if((dir = js_get_propertystr_cstring(ctx, obj, "dir"))) {
    dbuf_putstr(&db, dir);
    js_cstring_free(ctx, dir);
  } else if((root = js_get_propertystr_cstring(ctx, obj, "root"))) {
    dbuf_putstr(&db, root);
    js_cstring_free(ctx, root);
  }

  if(db.size)
    dbuf_putc(&db, PATHSEP_C);

  if((base = js_get_propertystr_cstring(ctx, obj, "base"))) {
    dbuf_putstr(&db, base);
    js_cstring_free(ctx, base);
  } else if((name = js_get_propertystr_cstring(ctx, obj, "name"))) {
    dbuf_putstr(&db, name);
    js_cstring_free(ctx, name);
    if((ext = js_get_propertystr_cstring(ctx, obj, "ext"))) {
      dbuf_putstr(&db, ext);
      js_cstring_free(ctx, ext);
    }
  }

  ret = JS_NewStringLen(ctx, (const char*)db.buf, db.size);
  dbuf_free(&db);

  return ret;
}

static JSValue
js_path_resolve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  const char* str;
  DynBuf db, cwd;
  ssize_t i;
  size_t len = 0;
  JSValue ret = JS_UNDEFINED;
  js_dbuf_init(ctx, &db);
  dbuf_0(&db);

  for(i = argc - 1; i >= 0; i--) {
    if(!JS_IsString(argv[i])) {
      ret = JS_ThrowTypeError(ctx, "argument #%zx is not a string", i);
      goto fail;
    }
    str = JS_ToCStringLen(ctx, &len, argv[i]);
    while(len > 0 && str[len - 1] == PATHSEP_C) len--;
    if(dbuf_reserve_start(&db, len + 1))
      goto fail;
    if(len > 0) {
      memcpy(db.buf, str, len);
      db.buf[len] = PATHSEP_C;
    }
    js_cstring_free(ctx, str);
  }

  if(!path_is_absolute_b((const char*)db.buf, db.size)) {
    js_dbuf_init(ctx, &cwd);
    str = path_getcwd(&cwd);
    len = cwd.size;
    if(dbuf_reserve_start(&db, len + 1))
      goto fail;
    if(len > 0) {
      memcpy(db.buf, str, len);
      db.buf[len] = PATHSEP_C;
    }
    dbuf_free(&cwd);
  }

  dbuf_0(&db);

  if(db.size) {
    db.size = path_collapse((char*)db.buf, db.size);
    while(db.size > 0 && db.buf[db.size - 1] == PATHSEP_C) db.size--;
    ret = JS_NewStringLen(ctx, (const char*)db.buf, db.size);
  }
fail:
  dbuf_free(&db);
  return ret;
}

static const JSCFunctionListEntry js_path_funcs[] = {
    JS_CFUNC_MAGIC_DEF("basename", 1, js_path_method, METHOD_BASENAME),
    JS_CFUNC_MAGIC_DEF("collapse", 1, js_path_method, METHOD_COLLAPSE),
    JS_CFUNC_MAGIC_DEF("dirname", 1, js_path_method, METHOD_DIRNAME),
    JS_CFUNC_MAGIC_DEF("exists", 1, js_path_method, METHOD_EXISTS),
    JS_CFUNC_MAGIC_DEF("extname", 1, js_path_method, METHOD_EXTNAME),
    JS_CFUNC_MAGIC_DEF("fnmatch", 1, js_path_method, METHOD_FNMATCH),
    JS_CFUNC_MAGIC_DEF("getcwd", 1, js_path_method, METHOD_GETCWD),
#ifndef __wasi__
    JS_CFUNC_MAGIC_DEF("gethome", 1, js_path_method, METHOD_GETHOME),
#endif
    JS_CFUNC_MAGIC_DEF("getsep", 1, js_path_method, METHOD_GETSEP),
    JS_CFUNC_MAGIC_DEF("isAbsolute", 1, js_path_method, METHOD_IS_ABSOLUTE),
    JS_CFUNC_MAGIC_DEF("isRelative", 1, js_path_method, METHOD_IS_RELATIVE),
    JS_CFUNC_MAGIC_DEF("isDirectory", 1, js_path_method, METHOD_IS_DIRECTORY),
    JS_CFUNC_MAGIC_DEF("isFile", 1, js_path_method, METHOD_IS_FILE),
    JS_CFUNC_MAGIC_DEF("isCharDev", 1, js_path_method, METHOD_IS_CHARDEV),
    JS_CFUNC_MAGIC_DEF("isBlockDev", 1, js_path_method, METHOD_IS_BLOCKDEV),
    JS_CFUNC_MAGIC_DEF("isFIFO", 1, js_path_method, METHOD_IS_FIFO),
    JS_CFUNC_MAGIC_DEF("isSocket", 1, js_path_method, METHOD_IS_SOCKET),
    JS_CFUNC_MAGIC_DEF("isSymlink", 1, js_path_method, METHOD_IS_SYMLINK),
    JS_CFUNC_MAGIC_DEF("isSeparator", 1, js_path_method, METHOD_IS_SEPARATOR),
    JS_CFUNC_MAGIC_DEF("length", 1, js_path_method, METHOD_LENGTH),
    JS_CFUNC_MAGIC_DEF("components", 1, js_path_method, METHOD_COMPONENTS),
    JS_CFUNC_MAGIC_DEF("readlink", 1, js_path_method, METHOD_READLINK),
#ifndef __wasi__
    JS_CFUNC_MAGIC_DEF("realpath", 1, js_path_method, METHOD_REALPATH),
#endif
    JS_CFUNC_MAGIC_DEF("right", 1, js_path_method, METHOD_RIGHT),
    JS_CFUNC_MAGIC_DEF("skip", 1, js_path_method, METHOD_SKIP),
    JS_CFUNC_MAGIC_DEF("skipSeparator", 1, js_path_method, METHOD_SKIP_SEPARATOR),
    JS_CFUNC_MAGIC_DEF("absolute", 1, js_path_method_dbuf, METHOD_ABSOLUTE),
    JS_CFUNC_MAGIC_DEF("append", 1, js_path_method_dbuf, METHOD_APPEND),
    JS_CFUNC_MAGIC_DEF("canonical", 1, js_path_method_dbuf, METHOD_CANONICAL),
    JS_CFUNC_MAGIC_DEF("concat", 2, js_path_method_dbuf, METHOD_CONCAT),
    JS_CFUNC_MAGIC_DEF("at", 2, js_path_method, METHOD_AT),
    JS_CFUNC_MAGIC_DEF("find", 2, js_path_method_dbuf, METHOD_FIND),
    JS_CFUNC_MAGIC_DEF("normalize", 1, js_path_method_dbuf, METHOD_NORMALIZE),
    JS_CFUNC_MAGIC_DEF("relative", 2, js_path_method_dbuf, METHOD_RELATIVE),
    JS_CFUNC_DEF("join", 1, js_path_join),
    JS_CFUNC_DEF("parse", 1, js_path_parse),
    JS_CFUNC_DEF("format", 1, js_path_format),
    JS_CFUNC_DEF("resolve", 1, js_path_resolve),
    JS_PROP_STRING_DEF("delimiter", PATHDELIM_S, JS_PROP_CONFIGURABLE),
    JS_PROP_STRING_DEF("sep", PATHSEP_S, JS_PROP_CONFIGURABLE),
};

static int
js_path_init(JSContext* ctx, JSModuleDef* m) {

  path_object = JS_NewObject(ctx);

  JS_SetPropertyFunctionList(ctx, path_object, js_path_funcs, countof(js_path_funcs));

  if(m) {
    JS_SetModuleExportList(ctx, m, js_path_funcs, countof(js_path_funcs));
    JS_SetModuleExport(ctx, m, "default", path_object);
  }
  return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_path
#endif

VISIBLE JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;
  m = JS_NewCModule(ctx, module_name, js_path_init);
  if(!m)
    return NULL;
  JS_AddModuleExportList(ctx, m, js_path_funcs, countof(js_path_funcs));
  JS_AddModuleExport(ctx, m, "default");
  return m;
}

/**
 * @}
 */
