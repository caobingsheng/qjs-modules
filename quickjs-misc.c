#include "include/defines.h"
#include <quickjs.h>
#include <quickjs-libc.h>
#include <quickjs-config.h>
#include "quickjs-misc.h"
#include "quickjs-internal.h"
#include "quickjs-location.h"
#include "quickjs-textcode.h"
#include "include/utils.h"
#include "include/path.h"
#include "include/base64.h"
#include <time.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif
#include <errno.h>
#ifdef HAVE_FNMATCH
#include <fnmatch.h>
#endif
#ifdef HAVE_GLOB
#include <glob.h>
#ifndef GLOB_MAGCHAR
#define GLOB_MAGCHAR 256
#endif
#ifndef GLOB_ALTDIRFUNC
#define GLOB_ALTDIRFUNC 512
#endif
#ifndef GLOB_BRACE
#define GLOB_BRACE 1024
#endif
#ifndef GLOB_NOMAGIC
#define GLOB_NOMAGIC 2048
#endif
#ifndef GLOB_TILDE
#define GLOB_TILDE 4096
#endif
#ifndef GLOB_ONLYDIR
#define GLOB_ONLYDIR 8192
#endif
#ifndef GLOB_TILDE_CHECK
#define GLOB_TILDE_CHECK 16384
#endif
#endif
#ifdef HAVE_WORDEXP
#include "wordexp.h"
#endif
#ifdef HAVE_INOTIFY
#include <sys/inotify.h>
#endif
#include "include/buffer-utils.h"
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#include <sys/ioctl.h>
#endif
#include "include/debug.h"

/**
 * \addtogroup quickjs-misc
 * @{
 */

#ifndef HAVE_MEMMEM
void* memmem(const void*, size_t, const void*, size_t);
#endif

enum {
  FUNC_GETEXECUTABLE = 0,
  FUNC_GETCWD,
  FUNC_GETROOT,
  FUNC_GETFD,
  FUNC_GETCOMMANDLINE,
  FUNC_GETPROCMAPS,
  FUNC_GETPROCMOUNTS,
  FUNC_GETPROCSTAT,
  FUNC_GETPID,
  FUNC_GETPPID,
  FUNC_GETSID,
  FUNC_GETUID,
  FUNC_GETGID,
  FUNC_GETEUID,
  FUNC_GETEGID,
  FUNC_SETUID,
  FUNC_SETGID,
  FUNC_SETEUID,
  FUNC_SETEGID
};

// static thread_local int inotify_fd = -1;

typedef struct pcg_state_setseq_64 {
  uint64_t state, inc;
} pcg32_random_t;

static pcg32_random_t pcg32_global = {0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL};

static inline uint32_t
pcg32_random_r(pcg32_random_t* rng) {
  uint64_t oldstate = rng->state;
  rng->state = oldstate * 6364136223846793005ULL + rng->inc;
  uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
  uint32_t rot = oldstate >> 59u;
  return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static uint32_t
pcg32_random(void) {
  return pcg32_random_r(&pcg32_global);
}

static void
pcg32_init_state(uint32_t state) {
  pcg32_global.state = ~(((uint64_t)state) << 32) | state;
}

static uint32_t
pcg32_random_bounded_divisionless(uint32_t range) {
  uint64_t random32bit, multiresult;
  uint32_t leftover;
  uint32_t threshold;
  random32bit = pcg32_random();
  multiresult = random32bit * range;
  leftover = (uint32_t)multiresult;
  if(leftover < range) {
    threshold = -range % range;
    while(leftover < threshold) {
      random32bit = pcg32_random();
      multiresult = random32bit * range;
      leftover = (uint32_t)multiresult;
    }
  }
  return multiresult >> 32; // [0, range)
}

static void
js_pointer_free_func(JSRuntime* rt, void* opaque, void* ptr) {
  js_free_rt(rt, ptr);
}

static void
js_string_free_func(JSRuntime* rt, void* opaque, void* ptr) {
  JSValue value = js_cstring_value(opaque);

  JS_FreeValueRT(rt, value);
}

static void
js_arraybuffer_free_func(JSRuntime* rt, void* opaque, void* ptr) {
  JSValue value = JS_MKPTR(JS_TAG_OBJECT, opaque);

  JS_FreeValueRT(rt, value);
}

static JSValue
js_misc_getrelease(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret;

  ret = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, ret, "name", JS_NewString(ctx, "quickjs"));
  JS_SetPropertyStr(ctx, ret, "sourceUrl", JS_NewString(ctx, "https://bellard.org/quickjs/quickjs-" CONFIG_VERSION ".tar.xz"));

  return ret;
}

static JSValue
js_misc_tostring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  //  JSValue arraybuffer_ctor = js_global_get_str(ctx, "ArrayBuffer");

  if(js_value_isclass(ctx, argv[0], JS_CLASS_ARRAY_BUFFER) || js_is_arraybuffer(ctx, argv[0]) /* || JS_IsInstanceOf(ctx, argv[0], arraybuffer_ctor)*/) {
    uint8_t* data;
    size_t len;

    if((data = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
      OffsetLength ol;

      js_offset_length(ctx, len, argc - 1, argv + 1, &ol);

      ret = JS_NewStringLen(ctx, (const char*)data + ol.offset, ol.length);
    }
  } else {
    ret = js_value_tostring(ctx, "Object", argc > 0 ? argv[0] : this_val);
  }

  //  JS_FreeValue(ctx, arraybuffer_ctor);

  return ret;
}

static JSValue
js_misc_topointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  void* ptr = 0;
  char buf[128];

  if(js_is_arraybuffer(ctx, argv[0]) || js_is_sharedarraybuffer(ctx, argv[0])) {
    size_t len;
    ptr = JS_GetArrayBuffer(ctx, &len, argv[0]);
  } else if(JS_IsString(argv[0])) {
    ptr = js_cstring_ptr(argv[0]);
  } else {
    switch(JS_VALUE_GET_TAG(argv[0])) {
        /*  case JS_TAG_BIG_DECIMAL:
          case JS_TAG_BIG_FLOAT:
          case JS_TAG_BIG_INT:
          case JS_TAG_FUNCTION_BYTECODE:
          case JS_TAG_INT:*/
      case JS_TAG_MODULE:
        /*      case JS_TAG_OBJECT:
               case JS_TAG_SYMBOL:*/
        {
          ptr = JS_VALUE_GET_PTR(argv[0]);
          break;
        }
      default: {
        return JS_ThrowTypeError(ctx, "toPointer: invalid type %s", js_value_typestr(ctx, argv[0]));
      }
    }
  }

  if(ptr) {
    snprintf(buf, sizeof(buf), "%p", ptr);
    ret = JS_NewString(ctx, buf);
  } else {
    ret = JS_NULL;
  }

  return ret;
}

static JSValue
js_misc_toarraybuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  MemoryBlock b;
  OffsetLength o;
  JSFreeArrayBufferDataFunc* f;
  void* opaque;

  /*  if(JS_IsString(argv[0])) {
      JSValueConst value = argv[0]; // JS_DupValue(ctx, argv[0]);
      b.base = JS_ToCStringLen(ctx, &b.size, value);
      f = &js_string_free_func;
      opaque = b.base;
      ret =
          JS_NewArrayBuffer(ctx, b.base + o.offset, MIN_NUM(b.size, o.length),
    js_string_free_func, (void*)b.base, FALSE); } else*/

  InputBuffer input = js_input_chars(ctx, argv[0]);
  js_offset_length(ctx, input.size, argc - 1, argv + 1, &o);
  b = input_buffer_block(&input);
  //    b = block_range(&b, &input.range);
  b = block_range(&b, &o);
  ret = js_arraybuffer_fromvalue(ctx, b.base, b.size, argv[0]);

  return ret;
}

static JSValue
js_misc_slice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  uint8_t* data;
  size_t len;

  if((data = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
    IndexRange ir;
    JSArrayBuffer* ab;
    if(!(ab = JS_GetOpaque(argv[0], JS_CLASS_ARRAY_BUFFER)))
      ab = JS_GetOpaque(argv[0], JS_CLASS_SHARED_ARRAY_BUFFER);

    js_index_range(ctx, len, argc - 1, argv + 1, &ir);

    JSValue value = JS_DupValue(ctx, argv[0]);
    JSObject* obj = JS_VALUE_GET_OBJ(value);
    return JS_NewArrayBuffer(ctx, data + ir.start, ir.end - ir.start, js_arraybuffer_free_func, (void*)obj, ab && ab->shared ? TRUE : FALSE);
  }

  return JS_ThrowTypeError(ctx, "argument 1 must be an ArrayBuffer");
}

static JSValue
js_misc_duparraybuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  uint8_t* data;
  size_t len;

  if((data = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
    OffsetLength ol;
    JSArrayBuffer* ab;
    if(!(ab = JS_GetOpaque(argv[0], JS_CLASS_ARRAY_BUFFER)))
      ab = JS_GetOpaque(argv[0], JS_CLASS_SHARED_ARRAY_BUFFER);

    js_offset_length(ctx, len, argc - 1, argv + 1, &ol);

    JSValue value = JS_DupValue(ctx, argv[0]);
    JSObject* obj = JS_VALUE_GET_OBJ(value);
    return JS_NewArrayBuffer(ctx, data + ol.offset, ol.length, js_arraybuffer_free_func, (void*)obj, ab && ab->shared ? TRUE : FALSE);
  }

  return JS_ThrowTypeError(ctx, "argument 1 must be an ArrayBuffer");
}

static JSValue
js_misc_resizearraybuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  JSObject* obj;
  JSArrayBuffer* arraybuf;
  uint64_t newlen;

  if(!js_is_arraybuffer(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "argument 1 must be an ArrayBuffer");

  obj = JS_VALUE_GET_OBJ(argv[0]);
  arraybuf = obj->u.array_buffer;
  JS_ToIndex(ctx, &newlen, argv[1]);

  if(arraybuf->shared)
    ret = JS_ThrowTypeError(ctx, "ArrayBuffer must not be shared");
  else if(arraybuf->shared)
    ret = JS_ThrowTypeError(ctx, "ArrayBuffer must have opaque == 0");
  else {
    arraybuf->data = js_realloc(ctx, arraybuf->data, newlen);
    arraybuf->byte_length = newlen;

    ret = JS_MKPTR(JS_TAG_OBJECT, obj);
  }

  return ret;
}

static JSValue
js_misc_concat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  int i;
  size_t total_len = 0, pos = 0;
  uint8_t* buf;
  InputBuffer* buffers = js_mallocz(ctx, sizeof(InputBuffer) * argc);
  for(i = 0; i < argc; i++) {
    buffers[i] = js_input_buffer(ctx, argv[i]);
    if(!buffers[i].data) {
      ret = JS_ThrowTypeError(ctx, "argument %d is not ArrayBuffer", i + 1);
      goto fail;
    }
    total_len += buffers[i].size;
  }
  buf = js_malloc(ctx, total_len);
  for(i = 0; i < argc; i++) {
    memcpy(&buf[pos], buffers[i].data, buffers[i].size);
    pos += buffers[i].size;
  }
  ret = JS_NewArrayBuffer(ctx, buf, total_len, js_pointer_free_func, 0, FALSE);
fail:
  for(i = 0; i < argc; i++)
    if(buffers[i].data)
      input_buffer_free(&buffers[i], ctx);
  return ret;
}

static JSValue
js_misc_searcharraybuffer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  MemoryBlock haystack, needle, mask;
  size_t n_size, h_end;

  if(!block_arraybuffer(&haystack, argv[0], ctx))
    return JS_ThrowTypeError(ctx, "argument 1 (haystack) must be an ArrayBuffer");
  if(!block_arraybuffer(&needle, argv[1], ctx))
    return JS_ThrowTypeError(ctx, "argument 2 (haystack) must be an ArrayBuffer");
  if(argc < 3) {
    uint8_t* ptr;
    ptrdiff_t ofs;

    if(needle.size <= haystack.size && (ptr = memmem(haystack.base, haystack.size, needle.base, needle.size))) {
      ofs = ptr - haystack.base;
      return JS_NewInt64(ctx, ofs);
    }

    return JS_NULL;
  }

  if(!block_arraybuffer(&mask, argv[2], ctx))
    return JS_ThrowTypeError(ctx, "argument 3 (mask) must be an ArrayBuffer");

  n_size = MIN_NUM(needle.size, mask.size);
  h_end = haystack.size - n_size;

  // naive searching algorithm (slow)
  for(size_t i = 0; i < h_end; i++) {
    int found = 1;
    for(size_t j = 0; j < n_size; j++) {
      if((haystack.base[i + j] ^ needle.base[j]) & mask.base[j]) {
        found = 0;
        break;
      }
    }
    if(found) {
      /*for(size_t j = 0; j < n_size; j++) {
        uint8_t xorval = haystack.base[i + j] ^ needle.base[j];
        printf("@(%zu + %zu); ", i, j);
        printf("%02x XOR %02x = %02x; ", haystack.base[i + j], needle.base[j], xorval);
        printf("%02x AND %02x = %02x\n", xorval, mask.base[j], xorval & mask.base[j]);
      }*/

      return JS_NewInt64(ctx, (int64_t)i);
    }
  }

  return JS_NULL;
}

static JSValue
js_misc_memcpy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  MemoryBlock dst = {0, 0}, src = {0, 0};
  OffsetLength s_offs, d_offs;

  int i = 0;

  if(!block_arraybuffer(&dst, argv[0], ctx))
    return JS_ThrowTypeError(ctx, "argument 1 (dst) must be an ArrayBuffer");

  i++;

  i += js_offset_length(ctx, dst.size, argc - i, argv + i, &s_offs);

  /* dst.base += s_offs.offset;
   dst.size -= s_offs.offset;
   dst.size = MIN_NUM(dst.size, s_offs.length);*/

  if(i == argc || !block_arraybuffer(&src, argv[i], ctx))
    return JS_ThrowTypeError(ctx, "argument %d (src) must be an ArrayBuffer", i + 1);

  i++;

  i += js_offset_length(ctx, dst.size, argc - i, argv + i, &d_offs);

  /* src.base += d_offs.offset;
   src.size -= d_offs.offset;
   src.size = MIN_NUM(src.size, d_offs.length);*/

  {
    size_t n = MIN_NUM(offset_size(&d_offs, block_length(&dst)), offset_size(&s_offs, block_length(&src)));

    if(n)
      memcpy(offset_data(&d_offs, block_data(&dst)), offset_data(&s_offs, block_data(&src)), n);

    return JS_NewInt64(ctx, n);
  }
}

#ifdef HAVE_FMEMOPEN
static JSValue
js_misc_fmemopen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  uint8_t* ptr;
  size_t len;
  int i = 0;
  const char* mode;

  if(!(ptr = JS_GetArrayBuffer(ctx, &len, argv[0])))
    return JS_ThrowTypeError(ctx, "argument 1 (dst) must be an ArrayBuffer");

  i++;

  if(i + 1 < argc && JS_IsNumber(argv[i])) {
    int64_t offset = 0;
    JS_ToInt64(ctx, &offset, argv[i++]);
    offset = MIN_NUM(len, offset);

    ptr += offset;
    len -= offset;
  }

  if(i + 1 < argc && JS_IsNumber(argv[i])) {
    int64_t length = 0;
    if(!JS_ToInt64(ctx, &length, argv[i++]))
      len = MIN_NUM(len, length);
  }

  {
    JSClassID class_id = js_class_find(ctx, "FILE");
    JSValue obj, proto = JS_GetClassProto(ctx, class_id);
    FILE* fp;
    JSSTDFile* file;
    mode = JS_ToCString(ctx, argv[0]);

    file = js_malloc(ctx, sizeof(JSSTDFile));
    *file = (JSSTDFile){0, TRUE, FALSE};

    file->f = fmemopen(ptr, len, mode);

    obj = JS_NewObjectProtoClass(ctx, proto, class_id);

    JS_SetOpaque(obj, file);

    return obj;
  }
}
#endif

static JSValue
js_misc_getperformancecounter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return JS_NewFloat64(ctx, (double)ts.tv_sec * 1000 + ((double)ts.tv_nsec / 1e06));
}

static JSValue
js_misc_proclink(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  DynBuf dbuf = {0};
  const char* link;
  char path[256];
  size_t n;
  ssize_t r;

  switch(magic) {
    case FUNC_GETEXECUTABLE: link = "exe"; break;
    case FUNC_GETCWD: link = "cwd"; break;
    case FUNC_GETROOT: link = "root"; break;
    case FUNC_GETFD: link = "fd/"; break;
  }

  n = snprintf(path, sizeof(path), "/proc/self/%s", link);

  if(magic == FUNC_GETFD) {
    int32_t fd;
    if(argc < 1 || !JS_IsNumber(argv[0]))
      return JS_ThrowTypeError(ctx, "argument 1 must be Number");

    JS_ToInt32(ctx, &fd, argv[0]);
    snprintf(&path[n], sizeof(path) - n, "%d", fd);
  }

  js_dbuf_init(ctx, &dbuf);

  if((r = path_readlink(path, &dbuf)) > 0) {
    ret = dbuf_tostring_free(&dbuf, ctx);
  } else if(r < 0) {
  }

  return ret;
}

static JSValue
js_misc_procread(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  DynBuf dbuf = {0};
  ssize_t i, j = 0, size, n;
  const char* file;
  char sep = '\n';

  switch(magic) {
    case FUNC_GETCOMMANDLINE:
      file = "/proc/self/cmdline";
      sep = '\0';
      break;
    case FUNC_GETPROCMAPS:
      file = "/proc/self/maps";
      sep = '\n';
      break;
    case FUNC_GETPROCMOUNTS:
      file = "/proc/self/mounts";
      sep = '\n';
      break;
    case FUNC_GETPROCSTAT:
      file = "/proc/self/stat";
      sep = ' ';
      break;
  }

  js_dbuf_init(ctx, &dbuf);

  if((size = dbuf_load(&dbuf, file)) > 0) {

    while(size > 0 && dbuf.buf[size - 1] == '\n') size--;

    ret = JS_NewArray(ctx);
    for(i = 0; i < size; i += n + 1) {
      size_t len;
      len = n = byte_chr(&dbuf.buf[i], size - i, sep);
      while(len > 0 && is_whitespace_char(dbuf.buf[i + len - 1])) len--;
      JS_SetPropertyUint32(ctx, ret, j++, JS_NewStringLen(ctx, (const char*)&dbuf.buf[i], len));
    }
  }

  dbuf_free(&dbuf);

  return ret;
}

static JSValue
js_misc_getprototypechain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue proto, prev = JS_UNDEFINED, ret;
  int64_t i = -1, j = 0, limit = -1, start = 0, end;

  if(argc < 1 || !JS_IsObject(argv[0]))
    return JS_ThrowTypeError(ctx, "argument 1 object excepted");

  if(argc >= 2 && !js_is_null_or_undefined(argv[1]))
    JS_ToInt64(ctx, &limit, argv[1]);

  if(argc >= 3 && !js_is_null_or_undefined(argv[2]))
    JS_ToInt64(ctx, &start, argv[2]);

  ret = JS_NewArray(ctx);
  end = limit >= 0 ? start + limit : -1;

  for(proto = JS_DupValue(ctx, argv[0]); !JS_IsException(proto) && !JS_IsNull(proto) && JS_IsObject(proto); proto = JS_GetPrototype(ctx, proto)) {
    BOOL circular = (JS_VALUE_GET_OBJ(proto) == JS_VALUE_GET_OBJ(prev));
    JS_FreeValue(ctx, prev);
    if(circular)
      break;
    if(i >= start && (end == -1 || i < end))
      JS_SetPropertyUint32(ctx, ret, j++, proto);
    ++i;
    prev = proto;
  }

  JS_FreeValue(ctx, proto);
  return ret;
}

static JSValue
js_misc_hrtime(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  struct timespec ts;
  JSValue ret;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  if(argc >= 1 && JS_IsArray(ctx, argv[0])) {
    int64_t sec, nsec;
    JSValue psec, pnsec;

    psec = JS_GetPropertyUint32(ctx, argv[0], 0);
    pnsec = JS_GetPropertyUint32(ctx, argv[0], 1);

    JS_ToInt64(ctx, &sec, psec);
    JS_ToInt64(ctx, &nsec, pnsec);
    JS_FreeValue(ctx, psec);
    JS_FreeValue(ctx, pnsec);

    if(nsec > ts.tv_nsec) {
      ts.tv_sec -= 1;
      ts.tv_nsec += 1000000000;
    }

    ts.tv_sec -= sec;
    ts.tv_nsec -= nsec;
  }

  ret = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, ret, 0, JS_NewInt64(ctx, ts.tv_sec));
  JS_SetPropertyUint32(ctx, ret, 1, JS_NewInt64(ctx, ts.tv_nsec));

  return ret;
}

#ifndef __wasi__
/*static JSValue
js_misc_realpath(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  char resolved[PATH_MAX];
  const char* path = JS_ToCString(ctx, argv[0]);
  char* result;

#ifndef __wasi__
  if((result = realpath(path, resolved)))
#endif
    return JS_NewString(ctx, result);
  return JS_NULL;
}*/
#ifdef USE_TEMPNAM
static JSValue
js_misc_tempnam(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  const char *dir = 0, *pfx = 0;
  char* nam;
  JSValue ret = JS_NULL;

  if(argc >= 1 && JS_IsString(argv[0]))
    dir = JS_ToCString(ctx, argv[0]);
  if(argc >= 2 && JS_IsString(argv[1]))
    pfx = JS_ToCString(ctx, argv[1]);

  if((nam = tempnam(dir, pfx))) {
    ret = JS_NewString(ctx, nam);
    free(nam);
  }
  return ret;
}
#endif

static JSValue
js_misc_mkstemp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  const char* tmp = 0;
  char* template;
  int fd;

  if(argc >= 1 && JS_IsString(argv[0]))
    tmp = JS_ToCString(ctx, argv[0]);

  template = js_strdup(ctx, tmp ? tmp : "/tmp/fileXXXXXX");

  if(tmp)
    JS_FreeCString(ctx, tmp);

  if(!template)
    return JS_ThrowOutOfMemory(ctx);

  fd = mkstemp(template);

  js_free(ctx, template);

  if(fd < 0) {
    fd = -errno;
    errno = 0;
  }

  return JS_NewInt32(ctx, fd);
}
#endif

#ifdef HAVE_FNMATCH
static JSValue
js_misc_fnmatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  size_t plen, slen;
  int32_t flags = 0, ret;

  const char* pattern = JS_ToCStringLen(ctx, &plen, argv[0]);
  const char* string = JS_ToCStringLen(ctx, &slen, argv[1]);

  if(argc >= 3)
    JS_ToInt32(ctx, &flags, argv[2]);

  ret = path_fnmatch(pattern, plen, string, slen, flags);
  JS_FreeCString(ctx, pattern);
  JS_FreeCString(ctx, string);
  return JS_NewBool(ctx, !ret);
}
#endif

#ifdef HAVE_GLOB
static JSContext* js_misc_glob_errfunc_ctx;
static JSValueConst js_misc_glob_errfunc_fn;

static int
js_misc_glob_errfunc(const char* epath, int eerrno) {
  JSContext* ctx;

  if((ctx = js_misc_glob_errfunc_ctx)) {
    JSValueConst argv[2] = {JS_NewString(ctx, epath), JS_NewInt32(ctx, eerrno)};

    JS_FreeValue(ctx, JS_Call(ctx, js_misc_glob_errfunc_fn, JS_NULL, 2, argv));

    JS_FreeValue(ctx, argv[0]);
    JS_FreeValue(ctx, argv[1]);
  }
  return 0;
}

static JSValue
js_misc_glob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  size_t start = 0, i;
  int32_t flags = 0;
  JSValue ret = JS_UNDEFINED;
  glob_t g = {0, 0, 0};
  int result;
  BOOL array_arg = FALSE;
  const char* pattern = JS_ToCString(ctx, argv[0]);

  if(argc >= 2)
    JS_ToInt32(ctx, &flags, argv[1]);

  if((array_arg = (argc >= 4 && JS_IsArray(ctx, argv[3])))) {
    ret = JS_DupValue(ctx, argv[3]);

    if(flags & GLOB_APPEND)
      start = js_array_length(ctx, ret);
  } else {
    ret = JS_NewArray(ctx);
  }

  js_misc_glob_errfunc_ctx = ctx;
  js_misc_glob_errfunc_fn = argc >= 3 ? argv[2] : JS_UNDEFINED;

  if((result = glob(pattern, flags & (~(GLOB_APPEND | GLOB_DOOFFS)), js_misc_glob_errfunc, &g)) == 0) {
    for(i = 0; i < g.gl_pathc; i++) JS_SetPropertyUint32(ctx, ret, i + start, JS_NewString(ctx, g.gl_pathv[i]));

    globfree(&g);
  }

  if(array_arg || result) {
    JS_FreeValue(ctx, ret);
    ret = JS_NewInt32(ctx, result);
  }

  JS_FreeValue(ctx, js_misc_glob_errfunc_fn);
  js_misc_glob_errfunc_ctx = 0;
  JS_FreeCString(ctx, pattern);
  return ret;
}
#endif

#ifdef HAVE_WORDEXP
static JSValue
js_misc_wordexp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  size_t start = 0, i;
  int32_t flags = 0;
  JSValue ret = JS_UNDEFINED;
  wordexp_t we = {0, 0, 0};
  int result;
  BOOL array_arg = FALSE;
  const char* s = JS_ToCString(ctx, argv[0]);

  if(argc >= 3)
    JS_ToInt32(ctx, &flags, argv[2]);

  if((array_arg = (argc >= 2 && JS_IsArray(ctx, argv[1])))) {
    ret = JS_DupValue(ctx, argv[1]);

    if(flags & WRDE_APPEND)
      start = js_array_length(ctx, ret);
  } else {
    ret = JS_NewArray(ctx);
  }

  if((result = wordexp(s, &we, flags & (~(WRDE_APPEND | WRDE_DOOFFS | WRDE_REUSE)))) == 0) {
    for(i = 0; i < we.we_wordc; i++) JS_SetPropertyUint32(ctx, ret, i + start, JS_NewString(ctx, we.we_wordv[i]));

    wordfree(&we);
  }

  if(array_arg || result) {
    JS_FreeValue(ctx, ret);
    ret = JS_NewInt32(ctx, result);
  }

  JS_FreeCString(ctx, s);
  return ret;
}
#endif

#ifndef _WIN32
static JSValue
js_misc_uname(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  struct utsname un;
  JSValue ret = JS_UNDEFINED;

  if(uname(&un) != -1) {
    ret = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, ret, "sysname", JS_NewString(ctx, un.sysname));
    JS_SetPropertyStr(ctx, ret, "nodename", JS_NewString(ctx, un.nodename));
    JS_SetPropertyStr(ctx, ret, "release", JS_NewString(ctx, un.release));
    JS_SetPropertyStr(ctx, ret, "version", JS_NewString(ctx, un.version));
    JS_SetPropertyStr(ctx, ret, "machine", JS_NewString(ctx, un.machine));
  }

  return ret;
}

static JSValue
js_misc_ioctl(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  int32_t fd = -1, args[2] = {-1, -1};
  int64_t request = -1LL;

  JS_ToInt32(ctx, &fd, argv[0]);
  JS_ToInt64(ctx, &request, argv[1]);

  if(argc >= 3)
    JS_ToInt32(ctx, &args[0], argv[2]);
  if(argc >= 4)
    JS_ToInt32(ctx, &args[1], argv[3]);

  return JS_NewInt32(ctx, ioctl(fd, request, args[0], args[1]));
}

static JSValue
js_misc_screensize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  int size[2] = {-1, -1};

  if(argc >= 1 && JS_IsObject(argv[0]))
    ret = JS_DupValue(ctx, argv[0]);

  if(!screen_size(size)) {
    JSValue width, height;
    if(JS_IsUndefined(ret))
      ret = argc >= 1 && JS_IsArray(ctx, argv[0]) ? JS_DupValue(ctx, argv[0]) : JS_NewArray(ctx);
    width = JS_NewInt32(ctx, size[0]);
    height = JS_NewInt32(ctx, size[1]);
    if(JS_IsArray(ctx, ret)) {
      JS_SetPropertyUint32(ctx, ret, 0, width);
      JS_SetPropertyUint32(ctx, ret, 1, height);
    } else if(JS_IsObject(ret)) {
      JS_SetPropertyStr(ctx, ret, "width", width);
      JS_SetPropertyStr(ctx, ret, "height", height);
    } else {
      JS_FreeValue(ctx, width);
      JS_FreeValue(ctx, height);
    }
  }

  return ret;
}
#endif

static JSValue
js_misc_btoa(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret;
  InputBuffer input = js_input_chars(ctx, argv[0]);
  size_t enclen = b64_get_encoded_buffer_size(input.size);
  uint8_t* encbuf = js_malloc(ctx, enclen);

  b64_encode(input.data, input.size, encbuf);

  ret = JS_NewStringLen(ctx, (const char*)encbuf, enclen);
  js_free(ctx, encbuf);
  return ret;
}

static JSValue
js_misc_atob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret;
  InputBuffer input = js_input_chars(ctx, argv[0]);
  size_t declen = b64_get_decoded_buffer_size(input.size);
  uint8_t* decbuf = js_malloc(ctx, declen);

  b64_decode(input.data, input.size, decbuf);

  ret = JS_NewArrayBufferCopy(ctx, (const uint8_t*)decbuf, declen);
  js_free(ctx, decbuf);
  return ret;
}

static JSValue
js_misc_compile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  const char* file = JS_ToCString(ctx, argv[0]);
  BOOL is_mod = FALSE;
  uint8_t* buf;
  size_t len;
  int32_t flags = JS_EVAL_TYPE_GLOBAL;

  if(argc >= 2 && JS_IsNumber(argv[1])) {
    JS_ToInt32(ctx, &flags, argv[1]);
  } else if(argc >= 2 && JS_IsBool(argv[1])) {
    if(JS_ToBool(ctx, argv[1]))
      flags |= JS_EVAL_TYPE_MODULE;
  }
  is_mod = !!(flags & JS_EVAL_TYPE_MODULE);
  if(str_ends(file, ".jsm"))
    is_mod = TRUE;
  if((buf = js_load_file(ctx, &len, file))) {
    if(!is_mod && JS_DetectModule((const char*)buf, len))
      is_mod = TRUE;
    flags |= (is_mod ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    ret = JS_Eval(ctx, (const char*)buf, len, file, flags | (is_mod ? JS_EVAL_FLAG_COMPILE_ONLY : 0));

    if(is_mod && !(flags & JS_EVAL_FLAG_COMPILE_ONLY)) {
      ret = JS_EvalFunction(ctx, ret);
    }
  } else {
    ret = JS_ThrowReferenceError(ctx, "could not load '%s': %s", file, strerror(errno));
  }
  return ret;
}

struct ImmutableClosure {
  JSRuntime* rt;
  JSValue ctor, proto;
};

static void
js_misc_immutable_free(void* ptr) {
  struct ImmutableClosure* closure = ptr;
  JS_FreeValueRT(closure->rt, closure->ctor);
  JS_FreeValueRT(closure->rt, closure->proto);
  free(ptr);
}

static JSValue
js_misc_immutable_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst argv[], int magic, void* ptr) {
  JSValue ret = JS_UNDEFINED;

  if(ptr) {
    struct ImmutableClosure* closure = ptr;

    ret = JS_CallConstructor2(ctx, closure->ctor, new_target, argc, argv);
  } else {
  }
  return ret;
}

static JSValue
js_misc_immutable_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  struct ImmutableClosure* closure;
  JSValue ret, proto;
  JSClassID id;
  JSCFunctionListEntry entry;
  char *name, *new_name;

  if(argc == 0 || !JS_IsConstructor(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "argument 1 must be a constructor");

  if(!(closure = malloc(sizeof(struct ImmutableClosure))))
    return JS_ThrowOutOfMemory(ctx);

  closure->rt = JS_GetRuntime(ctx);
  closure->ctor = JS_DupValue(ctx, argv[0]);
  closure->proto = JS_GetPropertyStr(ctx, closure->ctor, "prototype");

  if(JS_IsException(closure->proto)) {
    js_misc_immutable_free(closure);
    return JS_ThrowTypeError(ctx, "argument 1 must have a 'prototype' property");
  }

  name = js_object_classname(ctx, closure->proto);
  new_name = alloca(strlen(name) + sizeof("Immutable"));

  str_copy(&new_name[str_copy(new_name, "Immutable")], name);

  proto = JS_NewObject(ctx);
  JS_SetPrototype(ctx, proto, closure->proto);

  /* {
     JSCFunctionListEntry entries[] = {JS_PROP_STRING_DEF("[Symbol.toStringTag]", new_name, JS_PROP_CONFIGURABLE)};
     JS_SetPropertyFunctionList(ctx, proto, entries, countof(entries));
   }*/
  js_set_tostringtag_value(ctx, proto, JS_NewString(ctx, new_name));

  ret = JS_NewCClosure(ctx, js_misc_immutable_constructor, 0, 0, closure, js_misc_immutable_free);
  // ret = JS_NewCFunction2(ctx, js_misc_immutable_constructor, new_name, 1, JS_CFUNC_constructor, 0);

  if(!JS_IsConstructor(ctx, ret))
    JS_SetConstructorBit(ctx, ret, TRUE);

  JS_SetConstructor(ctx, ret, proto);

  return ret;
}

static JSValue
js_misc_write_object(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  size_t size;
  uint8_t* bytecode;

  if((bytecode = JS_WriteObject(ctx, &size, argv[0], JS_WRITE_OBJ_BYTECODE))) {
    ret = JS_NewArrayBuffer(ctx, bytecode, size, js_pointer_free_func, 0, FALSE);
  }
  return ret;
}

static JSValue
js_misc_read_object(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  InputBuffer input = js_input_chars(ctx, argv[0]);

  return JS_ReadObject(ctx, input.data, input.size, JS_READ_OBJ_BYTECODE);
}

static JSValue
js_misc_getx(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {

  int32_t ret = 0;

  switch(magic) {
#ifndef __wasi__
    case FUNC_GETPID: {
      ret = getpid();
      break;
    }
#endif
#if !defined(__wasi__) && !defined(_WIN32)
    case FUNC_GETPPID: {
      ret = getppid();
      break;
    }
#endif
    case FUNC_GETSID: {
      // sret = getsid();
      break;
    }
#if !defined(__wasi__) && !defined(_WIN32)
    case FUNC_GETUID: {
      ret = getuid();
      break;
    }
    case FUNC_GETGID: {
      ret = getgid();
      break;
    }
    case FUNC_GETEUID: {
      ret = geteuid();
      break;
    }
    case FUNC_GETEGID: {
      ret = getegid();
      break;
    }
    case FUNC_SETUID: {
      int32_t uid;
      JS_ToInt32(ctx, &uid, argv[0]);
      ret = setuid(uid);
      break;
    }
    case FUNC_SETGID: {
      int32_t gid;
      JS_ToInt32(ctx, &gid, argv[0]);
      ret = setgid(gid);
      break;
    }
    case FUNC_SETEUID: {
      int32_t euid;
      JS_ToInt32(ctx, &euid, argv[0]);
      ret = seteuid(euid);
      break;
    }
    case FUNC_SETEGID: {
      int32_t egid;
      JS_ToInt32(ctx, &egid, argv[0]);
      ret = setegid(egid);
      break;
    }
#endif
  }
  if(ret == -1)
    return JS_ThrowInternalError(ctx,
                                 "%s() failed: %s",
                                 ((const char* const[]){
                                     "getpid",
                                     "getppid",
                                     "getsid",
                                     "getuid",
                                     "getgid",
                                     "geteuid",
                                     "getegid",
                                     "setuid",
                                     "setgid",
                                     "seteuid",
                                     "setegid",
                                 })[magic - FUNC_GETPID],
                                 strerror(errno));

  return JS_NewInt32(ctx, ret);
}

enum {
  VALUE_TYPE = 0,
  VALUE_TAG,
  VALUE_PTR,
};

static JSValue
js_misc_valuetype(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;

  switch(magic) {
    case VALUE_TYPE: {
      const char* typestr = js_value_typestr(ctx, argv[0]);
      ret = JS_NewString(ctx, typestr);
      break;
    }
    case VALUE_TAG: {
      ret = JS_NewInt32(ctx, JS_VALUE_GET_TAG(argv[0]));
      break;
    }
    case VALUE_PTR: {
      void* ptr = JS_VALUE_GET_PTR(argv[0]);
      char buf[128];

      snprintf(buf, sizeof(buf), "%p", ptr);
      ret = JS_NewString(ctx, buf);
      break;
    }
  }
  return ret;
}

static JSValue
js_misc_evalbinary(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  BOOL load_only = FALSE;
  JSValueConst obj;
  int tag = JS_VALUE_GET_TAG(argv[0]);
  if(argc >= 2)
    load_only = JS_ToBool(ctx, argv[1]);

  if(tag != JS_TAG_MODULE && tag != JS_TAG_FUNCTION_BYTECODE)
    obj = js_misc_read_object(ctx, this_val, argc, argv);
  else
    obj = argv[0];
  if(JS_IsException(obj))
    return obj;

  tag = JS_VALUE_GET_TAG(obj);

  if(tag != JS_TAG_MODULE && tag != JS_TAG_FUNCTION_BYTECODE)
    return JS_ThrowTypeError(ctx, "obj is not MODULE nor BYTECODE");

  if(load_only) {
    if(tag == JS_TAG_MODULE)
      js_module_set_import_meta(ctx, obj, FALSE, FALSE);
  } else {
    if(tag == JS_TAG_MODULE) {
      if(JS_ResolveModule(ctx, obj) < 0) {
        JSModuleDef* m = JS_VALUE_GET_PTR(obj);
        const char* name = JS_AtomToCString(ctx, m->module_name);
        ret = JS_ThrowInternalError(ctx, "Failed resolving module '%s'", name);
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, obj);
        return ret;
      }
      js_module_set_import_meta(ctx, obj, FALSE, TRUE);
    }
    ret = JS_EvalFunction(ctx, obj);
  }
  return ret;
}

static JSValue
js_misc_opcode_array(JSContext* ctx, const JSOpCode* opcode) {
  JSValue ret = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, ret, 0, JS_NewUint32(ctx, opcode->size));
  JS_SetPropertyUint32(ctx, ret, 1, JS_NewUint32(ctx, opcode->n_pop));
  JS_SetPropertyUint32(ctx, ret, 2, JS_NewUint32(ctx, opcode->n_push));
  JS_SetPropertyUint32(ctx, ret, 3, JS_NewUint32(ctx, opcode->fmt));
  JS_SetPropertyUint32(ctx, ret, 4, JS_NewString(ctx, opcode->name));
  return ret;
}

static JSValue
js_misc_opcode_object(JSContext* ctx, const JSOpCode* opcode) {
  JSValue ret = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, ret, "size", JS_NewUint32(ctx, opcode->size));
  JS_SetPropertyStr(ctx, ret, "n_pop", JS_NewUint32(ctx, opcode->n_pop));
  JS_SetPropertyStr(ctx, ret, "n_push", JS_NewUint32(ctx, opcode->n_push));
  JS_SetPropertyStr(ctx, ret, "fmt", JS_NewUint32(ctx, opcode->fmt));
  JS_SetPropertyStr(ctx, ret, "name", JS_NewString(ctx, opcode->name));
  return ret;
}

static JSValue
js_misc_opcodes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_NewArray(ctx);
  size_t i, j, len = countof(js_opcodes);
  BOOL as_object = FALSE;

  if(argc >= 1)
    as_object = JS_ToBool(ctx, argv[0]);

  for(i = 0, j = 0; i < len; i++) {

    if(i >= OP_TEMP_START && i < OP_TEMP_END)
      continue;

    JS_SetPropertyUint32(ctx, ret, j++, (as_object ? js_misc_opcode_object : js_misc_opcode_array)(ctx, &js_opcodes[i]));
  }

  return ret;
}

static JSValue
js_misc_get_bytecode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;

  if(JS_IsFunction(ctx, argv[0])) {
    JSObject* obj = JS_VALUE_GET_OBJ(argv[0]);
    JSFunctionBytecode* fnbc;

    if((fnbc = obj->u.func.function_bytecode)) {
      ret = JS_NewArrayBufferCopy(ctx, fnbc->byte_code_buf, fnbc->byte_code_len);
    }
  }

  return ret;
}

enum {
  ATOM_TO_STRING = 0,
  ATOM_TO_VALUE,
  VALUE_TO_ATOM,
};

static JSValue
js_misc_atom(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;

  switch(magic) {
    case ATOM_TO_STRING: {
      int32_t atom;
      JS_ToInt32(ctx, &atom, argv[0]);
      ret = JS_AtomToString(ctx, atom);
      break;
    }
    case ATOM_TO_VALUE: {
      int32_t atom;
      JS_ToInt32(ctx, &atom, argv[0]);
      ret = JS_AtomToValue(ctx, atom);
      break;
    }
    case VALUE_TO_ATOM: {
      JSAtom atom = JS_ValueToAtom(ctx, argv[0]);
      ret = JS_NewUint32(ctx, atom);
      break;
    }
  }
  return ret;
}

enum {
  GET_CLASS_ID = 0,
  GET_CLASS_NAME,
  GET_CLASS_ATOM,
  GET_CLASS_COUNT,
  GET_CLASS_PROTO,
  GET_CLASS_CONSTRUCTOR,
  GET_TYPE_ID,
  GET_TYPE_STR,
  GET_TYPE_NAME,
};

static JSValue
js_misc_classid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  JSObject* obj;
  int32_t class_id = 0;

  if(argc >= 1) {
    if(JS_IsNumber(argv[0]))
      JS_ToInt32(ctx, &class_id, argv[0]);
    else if((obj = js_value_obj(argv[0])))
      class_id = obj->class_id;
    else if(JS_IsObject(argv[0]))
      class_id = JS_GetClassID(argv[0]);
  }

  switch(magic) {
    case GET_CLASS_ID: {
      if(class_id > 0)
        ret = JS_NewUint32(ctx, class_id);
      break;
    }
    case GET_CLASS_NAME: {
      if(class_id > 0) {
        JSAtom atom;
        if((atom = js_class_atom(ctx, class_id)))
          ret = JS_AtomToValue(ctx, atom);
      }
      break;
    }
    case GET_CLASS_ATOM: {
      if(class_id > 0) {
        JSAtom atom;
        if((atom = js_class_atom(ctx, class_id)))
          ret = JS_NewInt32(ctx, atom);
      }
      break;
    }
    case GET_CLASS_COUNT: {
      uint32_t i, class_count = ctx->rt->class_count;
      for(i = 1; i < class_count; i++)
        if(!JS_IsRegisteredClass(ctx->rt, i))
          break;

      ret = JS_NewUint32(ctx, i);
      break;
    }
    case GET_CLASS_PROTO: {
      if(class_id > 0)
        ret = JS_GetClassProto(ctx, class_id);
      break;
    }
    case GET_CLASS_CONSTRUCTOR: {
      if(class_id > 0) {
        JSValue proto = JS_GetClassProto(ctx, class_id);
        if(JS_IsObject(proto))
          ret = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
      }
      break;
    }
  }
  return ret;
}

static JSValue
js_misc_type(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  int32_t type_id = 0;

  if(argc >= 1) {
    /*  if(JS_IsNumber(argv[0]))
          JS_ToInt32(ctx, &type_id, argv[0]);
        else*/
    type_id = js_value_type(ctx, argv[0]);
  }

  switch(magic) {
    case GET_TYPE_ID: {
      ret = JS_NewInt32(ctx, type_id);
      break;
    }
    case GET_TYPE_STR: {
      const char* type;
      if((type = js_value_type_name(type_id)))
        ret = JS_NewString(ctx, type);
      break;
    }
    case GET_TYPE_NAME: {
      const char* type;
      if((type = (const char*)js_object_classname(ctx, argv[0]))) {
        ret = JS_NewString(ctx, type);
        js_free(ctx, (void*)type);
      } else if((type = js_value_type_name(type_id))) {
        ret = JS_NewString(ctx, type);
      }
      break;
    }
  }
  return ret;
}

enum {
  BITFIELD_SET,
  BITFIELD_BITS,
  BITFIELD_FROMARRAY,
  BITFIELD_TOARRAY,
};

static JSValue
js_misc_bitfield(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  size_t len;
  int64_t offset = 0;
  switch(magic) {
    case BITFIELD_SET: {
      const uint8_t* buf;

      if(argc >= 2)
        JS_ToInt64(ctx, &offset, argv[1]);

      if((buf = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
        size_t i, j = 0, bits = len * 8;
        ret = JS_NewArray(ctx);

        for(i = 0; i < bits; i++) {
          if(buf[i >> 3] & (1u << (i & 0x7))) {
            JS_SetPropertyUint32(ctx, ret, j++, JS_NewInt64(ctx, i + offset));
          }
        }
      }
      break;
    }

    case BITFIELD_BITS: {
      const uint8_t* buf;

      if(argc >= 2)
        JS_ToInt64(ctx, &offset, argv[1]);

      if(argc >= 1 && (buf = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
        size_t i, j = 0, bits = len * 8;
        ret = JS_NewArray(ctx);

        for(i = 0; i < bits; i++) {
          BOOL value = !!(buf[i >> 3] & (1u << (i & 0x7)));
          JS_SetPropertyUint32(ctx, ret, j++, JS_NewInt32(ctx, value));
        }
      } else if(argc >= 1 && JS_IsArray(ctx, argv[0])) {

        size_t i, len = js_array_length(ctx, argv[0]);
        uint8_t* bufptr;
        size_t bufsize = (len + 7) >> 3;

        if((bufptr = js_mallocz(ctx, bufsize)) == 0)
          return JS_ThrowOutOfMemory(ctx);

        for(i = 0; i < len; i++) {
          JSValue element = JS_GetPropertyUint32(ctx, argv[0], i);
          BOOL value = JS_ToBool(ctx, element);
          JS_FreeValue(ctx, element);

          if(value)
            bufptr[i >> 3] |= 1u << (i & 0x7);
        }
        ret = JS_NewArrayBuffer(ctx, bufptr, bufsize, js_pointer_free_func, bufptr, FALSE);
      }
      break;
    }
    case BITFIELD_TOARRAY: {
      const uint8_t* buf;

      if(argc >= 2)
        JS_ToInt64(ctx, &offset, argv[1]);

      if((buf = JS_GetArrayBuffer(ctx, &len, argv[0]))) {
        size_t i, j = 0, bits = len * 8;
        ret = JS_NewArray(ctx);

        for(i = 0; i < bits; i++) {
          BOOL value = buf[i >> 3] & (1u << (i & 0x7));

          JS_SetPropertyUint32(ctx, ret, i, JS_NewBool(ctx, value));
        }
      }
      break;
    }
    case BITFIELD_FROMARRAY: {
      JSValue prop;
      if(argc >= 2)
        JS_ToInt64(ctx, &offset, argv[1]);

      if(!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "argument must be an array");

      prop = JS_GetPropertyUint32(ctx, argv[0], 0);
      len = js_array_length(ctx, argv[0]);
      if(len) {
        uint8_t* bufptr;
        size_t bufsize;

        if(JS_IsBool(prop)) {
          size_t i;
          bufsize = (len + 7) >> 3;
          if((bufptr = js_mallocz(ctx, bufsize)) == 0)
            return JS_ThrowOutOfMemory(ctx);

          for(i = 0; i < len; i++) {
            JSValue value = JS_GetPropertyUint32(ctx, argv[0], i);
            BOOL b = JS_ToBool(ctx, value);
            JS_FreeValue(ctx, value);

            bufptr[i >> 3] |= (b ? 1 : 0) << (i & 0x7);
          }

        } else {

          size_t i;
          int64_t max = -1;

          for(i = 0; i < len; i++) {
            JSValue value = JS_GetPropertyUint32(ctx, argv[0], i);
            uint32_t number;
            JS_ToUint32(ctx, &number, value);
            JS_FreeValue(ctx, value);

            if(max < number)
              max = number;
          }
          bufsize = ((max + 1) + 7) >> 3;
          if((bufptr = js_mallocz(ctx, bufsize)) == 0)
            return JS_ThrowOutOfMemory(ctx);

          for(i = 0; i < len; i++) {
            JSValue value = JS_GetPropertyUint32(ctx, argv[0], i);
            uint32_t number;
            JS_ToUint32(ctx, &number, value);
            JS_FreeValue(ctx, value);

            number -= offset;

            bufptr[number >> 3] |= 1u << (number & 0x7);
          }
        }
        ret = JS_NewArrayBuffer(ctx, bufptr, bufsize, js_pointer_free_func, bufptr, FALSE);
      }
      break;
    }
  }
  return ret;
}
enum {
  BITOP_NOT,
  BITOP_XOR,
  BITOP_AND,
  BITOP_OR,
};

static JSValue
js_misc_bitop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue ret = JS_UNDEFINED;
  size_t i;
  struct {
    uint8_t* buf;
    size_t len;
  } ab[2] = {{0, 0}, {0, 0}};

  if(argc >= 1) {
    ab[0].buf = JS_GetArrayBuffer(ctx, &ab[0].len, argv[0]);

    if(argc >= 2)
      ab[1].buf = JS_GetArrayBuffer(ctx, &ab[1].len, argv[1]);
  }

  if(ab[0].buf == 0)
    return JS_ThrowTypeError(ctx, "argument 1 must be an ArrayBuffer");

  if(magic > BITOP_NOT && ab[1].buf == 0)
    return JS_ThrowTypeError(ctx, "argument 2 must be an ArrayBuffer");

  ret = JS_DupValue(ctx, argv[0]);

  switch(magic) {
    case BITOP_NOT: {
      for(i = 0; i < ab[0].len; i++) ab[0].buf[i] ^= 0xffu;

      break;
    }
    case BITOP_XOR: {

      for(i = 0; i < ab[0].len; i++) ab[0].buf[i] ^= ab[1].buf[i % ab[1].len];

      break;
    }
    case BITOP_AND: {
      break;
    }
    case BITOP_OR: {
      break;
    }
  }
  return ret;
}

enum {
  RANDOM_RAND,
  RANDOM_RANDI,
  RANDOM_RANDF,
  RANDOM_SRAND,
};

static JSValue
js_misc_random(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  uint32_t bound = 0;
  JSValue ret = JS_UNDEFINED;

  if(argc > 0 && JS_IsNumber(argv[0]))
    JS_ToUint32(ctx, &bound, argv[0]);

  switch(magic) {
    case RANDOM_RAND: {
      uint32_t num = argc > 0 ? pcg32_random_bounded_divisionless(bound) : pcg32_random();
      ret = JS_NewUint32(ctx, num);
      break;
    }
    case RANDOM_RANDI: {
      int32_t num = argc > 0 ? pcg32_random_bounded_divisionless(bound * 2) - bound : pcg32_random();
      ret = JS_NewInt32(ctx, num);
      break;
    }
    case RANDOM_RANDF: {
      uint32_t num = pcg32_random();
      ret = JS_NewFloat64(ctx, (double)num / UINT32_MAX);
      break;
    }
    case RANDOM_SRAND: {
      uint32_t st = 0;
      JS_ToUint32(ctx, &st, argv[0]);
      pcg32_init_state(st);
      ret = JS_UNDEFINED;
      break;
    }
  }

  return ret;
}

JSValue
js_misc_escape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  InputBuffer input = js_input_chars(ctx, argv[0]);
  if(input.data) {
    DynBuf output;
    js_dbuf_init(ctx, &output);
    dbuf_put_escaped(&output, (const char*)input.data, input.size);
    return dbuf_tostring_free(&output, ctx);
  }
  return JS_DupValue(ctx, argv[0]);
}

JSValue
js_misc_quote(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  InputBuffer input = js_input_chars(ctx, argv[0]);
  DynBuf output;
  char quote = '"';
  uint8_t table[256] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 'b',  't',  'n',  'v',  'f',  'r',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, '\\', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75,
      0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75, 0x75,
  };

  js_dbuf_init(ctx, &output);
  if(argc >= 2) {
    const char* str = JS_ToCString(ctx, argv[1]);
    if(str[0])
      quote = str[0];
    JS_FreeCString(ctx, str);
  }

  table[(unsigned)quote] = quote;

  if(quote == '`') {
    table[(unsigned)'\r'] = 0;
    table[(unsigned)'\n'] = 0;
    table[(unsigned)'$'] = '$';
  }

  dbuf_putc(&output, quote);
  dbuf_put_escaped_table(&output, (const char*)input.data, input.size, table);
  dbuf_putc(&output, quote);
  return dbuf_tostring_free(&output, ctx);
}

JSValue
js_misc_error(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  int32_t errnum = errno;
  const char* syscall = 0;
  JSValue err;
  if(argc >= 1)
    JS_ToInt32(ctx, &errnum, argv[0]);
  if(argc >= 2)
    syscall = JS_ToCString(ctx, argv[1]);

  err = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, err, "errno", JS_NewInt32(ctx, errnum));
  JS_SetPropertyStr(ctx, err, "syscall", JS_NewString(ctx, syscall));

  if(syscall)
    JS_FreeCString(ctx, syscall);

  return err;
}
enum {
  IS_ARRAY,
  IS_BIGDECIMAL,
  IS_BIGFLOAT,
  IS_BIGINT,
  IS_BOOL,
  IS_CFUNCTION,
  IS_CONSTRUCTOR,
  IS_EMPTYSTRING,
  IS_ERROR,
  IS_EXCEPTION,
  IS_EXTENSIBLE,
  IS_FUNCTION,
  IS_HTMLDDA,
  IS_INSTANCEOF,
  IS_INTEGER,
  IS_JOBPENDING,
  IS_LIVEOBJECT,
  IS_NULL,
  IS_NUMBER,
  IS_OBJECT,
  IS_REGISTEREDCLASS,
  IS_STRING,
  IS_SYMBOL,
  IS_UNCATCHABLEERROR,
  IS_UNDEFINED,
  IS_UNINITIALIZED,
  IS_ARRAYBUFFER,
};

JSValue
js_misc_is(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  int32_t r = -1;
  JSValueConst arg = argc >= 1 ? argv[0] : JS_UNDEFINED;

  switch(magic) {
    case IS_ARRAY: r = JS_IsArray(ctx, arg); break;
    case IS_BIGDECIMAL: r = JS_IsBigDecimal(arg); break;
    case IS_BIGFLOAT: r = JS_IsBigFloat(arg); break;
    case IS_BIGINT: r = JS_IsBigInt(ctx, arg); break;
    case IS_BOOL: r = JS_IsBool(arg); break;
    case IS_CFUNCTION: r = JS_GetClassID(arg) == JS_CLASS_C_FUNCTION; break;
    case IS_CONSTRUCTOR: r = JS_IsConstructor(ctx, arg); break;
    case IS_EMPTYSTRING: r = JS_VALUE_GET_TAG(arg) == JS_TAG_STRING && JS_VALUE_GET_STRING(arg)->len == 0; break;
    case IS_ERROR: r = JS_IsError(ctx, arg); break;
    case IS_EXCEPTION: r = JS_IsException(arg); break;
    case IS_EXTENSIBLE: r = JS_IsExtensible(ctx, arg); break;
    case IS_FUNCTION: r = JS_IsFunction(ctx, arg); break;
    case IS_HTMLDDA: r = JS_VALUE_GET_TAG(arg) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(arg)->is_HTMLDDA; break;
    case IS_INSTANCEOF: r = JS_IsInstanceOf(ctx, arg, argv[1]); break;
    case IS_INTEGER: r = JS_IsNumber(arg) && JS_VALUE_GET_TAG(arg) != JS_TAG_FLOAT64; break;
    case IS_JOBPENDING: r = JS_IsJobPending(JS_GetRuntime(ctx)); break;
    case IS_LIVEOBJECT: r = JS_IsLiveObject(JS_GetRuntime(ctx), arg); break;
    case IS_NULL: r = JS_IsNull(arg); break;
    case IS_NUMBER: r = JS_IsNumber(arg); break;
    case IS_OBJECT: r = JS_IsObject(arg); break;
    case IS_REGISTEREDCLASS: r = !JS_ToInt32(ctx, &r, arg) && JS_IsRegisteredClass(JS_GetRuntime(ctx), r); break;
    case IS_STRING: r = JS_IsString(arg); break;
    case IS_SYMBOL: r = JS_IsSymbol(arg); break;
    case IS_UNCATCHABLEERROR: r = JS_IsUncatchableError(ctx, arg); break;
    case IS_UNDEFINED: r = JS_IsUndefined(arg); break;
    case IS_UNINITIALIZED: r = JS_IsUninitialized(arg); break;
    case IS_ARRAYBUFFER: r = js_is_arraybuffer(ctx, arg); break;
  }
  if(r == -1)
    return JS_ThrowInternalError(ctx, "js_misc_is %d", magic);
  return JS_NewBool(ctx, r >= 1);
}

#ifdef HAVE_INOTIFY
static JSValue
js_misc_watch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret = JS_UNDEFINED;
  int32_t fd = -1;

  if(argc >= 1) {
    JS_ToInt32(ctx, &fd, argv[0]);
  }

  if(argc >= 2 && JS_IsString(argv[1])) {
    int wd;
    int32_t flags = IN_ALL_EVENTS;
    const char* filename;

    filename = JS_ToCString(ctx, argv[1]);
    if(argc >= 3)
      JS_ToInt32(ctx, &flags, argv[2]);

    if((wd = inotify_add_watch(fd, filename, flags)) == -1)
      return JS_ThrowInternalError(ctx, "inotify_add_watch(%d, %s, %08x) = %d (%s)", fd, filename, flags, wd, strerror(errno));

    // printf("inotify_add_watch(%d, %s, %08x) = %d\n", fd, filename, flags, wd);

    ret = JS_NewInt32(ctx, wd);
  } else if(argc >= 2 && JS_IsNull(argv[1])) {
    int r;
    int32_t wd = -1;

    JS_ToInt32(ctx, &wd, argv[0]);

    if((r = inotify_rm_watch(fd, wd)) == -1)
      return JS_ThrowInternalError(ctx, "inotify_rm_watch(%d, %d) = %d (%s)", fd, wd, r, strerror(errno));
    // printf("inotify_add_watch(%d, %d) = %d\n", fd, wd, r);

    ret = JS_NewInt32(ctx, r);
  } else {
    int fd;
    if((fd = inotify_init1(IN_NONBLOCK)) == -1)
      return JS_ThrowInternalError(ctx, "inotify_init1(IN_NONBLOCK) failed (%s)", strerror(errno));

    // printf("inotify_init1() = %d\n", fd);
    ret = JS_NewInt32(ctx, fd);
  }

  return ret;
}
#endif

#ifdef HAVE_DAEMON
static JSValue
js_misc_daemon(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  BOOL nochdir, noclose;

  nochdir = argc >= 1 && JS_ToBool(ctx, argv[0]);

  noclose = argc >= 2 && JS_ToBool(ctx, argv[0]);

  return JS_NewInt32(ctx, daemon(nochdir, noclose));
}
#endif

#ifdef HAVE_FORK
static JSValue
js_misc_fork(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  return JS_NewInt32(ctx, fork());
}
#endif

#ifdef HAVE_VFORK
static JSValue
js_misc_vfork(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  return JS_NewInt32(ctx, vfork());
}
#endif

#ifdef HAVE_SETSID
static JSValue
js_misc_setsid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  return JS_NewInt32(ctx, setsid());
}
#endif

typedef struct {
  JSContext* ctx;
  JSValue fn;
} JSAtExitEntry;

thread_local Vector js_misc_atexit_functions;

static void
js_misc_atexit_handler() {
  JSAtExitEntry* entry;

  vector_foreach_t(&js_misc_atexit_functions, entry) {
    JSValue ret = JS_Call(entry->ctx, entry->fn, JS_UNDEFINED, 0, 0);
    JS_FreeValue(entry->ctx, ret);
  }
}

static JSValue
js_misc_atexit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSAtExitEntry entry;

  if(argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "argument 1 must be function");

  entry.ctx = ctx;
  entry.fn = JS_DupValue(ctx, argv[0]);

  vector_push(&js_misc_atexit_functions, entry);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_misc_funcs[] = {
    JS_CFUNC_DEF("getRelease", 0, js_misc_getrelease),
#ifndef __wasi__
// JS_CFUNC_DEF("realpath", 1, js_misc_realpath),
#ifdef USE_TEMPNAM
    JS_CFUNC_DEF("tempnam", 0, js_misc_tempnam),
#endif
    JS_CFUNC_DEF("mkstemp", 1, js_misc_mkstemp),
#endif
#ifdef HAVE_FNMATCH
    JS_CFUNC_DEF("fnmatch", 3, js_misc_fnmatch),
#endif
#ifdef HAVE_GLOB
    JS_CFUNC_DEF("glob", 2, js_misc_glob),
#endif
#ifdef HAVE_WORDEXP
    JS_CFUNC_DEF("wordexp", 2, js_misc_wordexp),
#endif
#ifdef HAVE_INOTIFY
    JS_CFUNC_DEF("watch", 2, js_misc_watch),
#endif
#ifdef HAVE_DAEMON
    JS_CFUNC_DEF("daemon", 2, js_misc_daemon),
#endif
#ifdef HAVE_FORK
    JS_CFUNC_DEF("fork", 0, js_misc_fork),
#endif
#ifdef HAVE_VFORK
    JS_CFUNC_DEF("vfork", 0, js_misc_vfork),
#endif
#ifdef HAVE_SETSID
    JS_CFUNC_DEF("setsid", 0, js_misc_setsid),
#endif
    JS_CFUNC_DEF("atexit", 1, js_misc_atexit),
    JS_CFUNC_DEF("toString", 1, js_misc_tostring),
    JS_CFUNC_DEF("toPointer", 1, js_misc_topointer),
    JS_CFUNC_DEF("toArrayBuffer", 1, js_misc_toarraybuffer),
    JS_CFUNC_DEF("dupArrayBuffer", 1, js_misc_duparraybuffer),
    JS_CFUNC_DEF("sliceArrayBuffer", 1, js_misc_slice),
    JS_CFUNC_DEF("resizeArrayBuffer", 1, js_misc_resizearraybuffer),
    JS_CFUNC_DEF("concat", 1, js_misc_concat),
    JS_CFUNC_DEF("searchArrayBuffer", 2, js_misc_searcharraybuffer),
    JS_CFUNC_DEF("memcpy", 2, js_misc_memcpy),
#ifdef HAVE_FMEMOPEN
    JS_CFUNC_DEF("fmemopen", 2, js_misc_fmemopen),
#endif
    JS_CFUNC_DEF("getPerformanceCounter", 0, js_misc_getperformancecounter),
    JS_CFUNC_MAGIC_DEF("getExecutable", 0, js_misc_proclink, FUNC_GETEXECUTABLE),
    JS_CFUNC_MAGIC_DEF("getCurrentWorkingDirectory", 0, js_misc_proclink, FUNC_GETCWD),
    JS_CFUNC_MAGIC_DEF("getRootDirectory", 0, js_misc_proclink, FUNC_GETROOT),
    JS_CFUNC_MAGIC_DEF("getFileDescriptor", 0, js_misc_proclink, FUNC_GETFD),
    JS_CFUNC_MAGIC_DEF("getCommandLine", 0, js_misc_procread, FUNC_GETCOMMANDLINE),
    JS_CFUNC_MAGIC_DEF("getProcMaps", 0, js_misc_procread, FUNC_GETPROCMAPS),
    JS_CFUNC_MAGIC_DEF("getProcMounts", 0, js_misc_procread, FUNC_GETPROCMOUNTS),
    JS_CFUNC_MAGIC_DEF("getProcStat", 0, js_misc_procread, FUNC_GETPROCSTAT),
    JS_CFUNC_DEF("getPrototypeChain", 0, js_misc_getprototypechain),
#ifndef __wasi__
    JS_CFUNC_MAGIC_DEF("getpid", 0, js_misc_getx, FUNC_GETPID),
    JS_CFUNC_MAGIC_DEF("getppid", 0, js_misc_getx, FUNC_GETPPID),
#endif
    JS_CFUNC_MAGIC_DEF("getsid", 0, js_misc_getx, FUNC_GETSID),
#if !defined(__wasi__) && !defined(_WIN32)
    JS_CFUNC_MAGIC_DEF("getuid", 0, js_misc_getx, FUNC_GETUID),
    JS_CFUNC_MAGIC_DEF("getgid", 0, js_misc_getx, FUNC_GETGID),
    JS_CFUNC_MAGIC_DEF("geteuid", 0, js_misc_getx, FUNC_GETEUID),
    JS_CFUNC_MAGIC_DEF("getegid", 0, js_misc_getx, FUNC_GETEGID),
    JS_CFUNC_MAGIC_DEF("setuid", 1, js_misc_getx, FUNC_SETUID),
    JS_CFUNC_MAGIC_DEF("setgid", 1, js_misc_getx, FUNC_SETGID),
#endif
    JS_CFUNC_MAGIC_DEF("seteuid", 1, js_misc_getx, FUNC_SETEUID),
    JS_CFUNC_MAGIC_DEF("setegid", 1, js_misc_getx, FUNC_SETEGID),
    JS_CFUNC_DEF("hrtime", 0, js_misc_hrtime),
#ifndef _WIN32
    JS_CFUNC_DEF("uname", 0, js_misc_uname),
#endif
#ifdef HAVE_TERMIOS_H
    JS_CFUNC_DEF("ioctl", 3, js_misc_ioctl),
    JS_CFUNC_DEF("getScreenSize", 0, js_misc_screensize),
#endif
    JS_CFUNC_DEF("btoa", 1, js_misc_btoa),
    JS_CFUNC_DEF("atob", 1, js_misc_atob),
    JS_CFUNC_MAGIC_DEF("not", 1, js_misc_bitop, BITOP_NOT),
    JS_CFUNC_MAGIC_DEF("xor", 2, js_misc_bitop, BITOP_XOR),
    JS_CFUNC_MAGIC_DEF("and", 2, js_misc_bitop, BITOP_AND),
    JS_CFUNC_MAGIC_DEF("or", 2, js_misc_bitop, BITOP_OR),
    JS_CFUNC_MAGIC_DEF("bitfieldSet", 1, js_misc_bitfield, BITFIELD_SET),
    JS_CFUNC_MAGIC_DEF("bits", 1, js_misc_bitfield, BITFIELD_BITS),
    JS_CFUNC_MAGIC_DEF("bitfieldToArray", 1, js_misc_bitfield, BITFIELD_TOARRAY),
    JS_CFUNC_MAGIC_DEF("arrayToBitfield", 1, js_misc_bitfield, BITFIELD_FROMARRAY),
    JS_CFUNC_MAGIC_DEF("compileScript", 1, js_misc_compile, 0),
    JS_CFUNC_MAGIC_DEF("evalScript", 1, js_misc_compile, 1),
    JS_CFUNC_MAGIC_DEF("immutableClass", 1, js_misc_immutable_class, 1),
    JS_CFUNC_DEF("writeObject", 1, js_misc_write_object),
    JS_CFUNC_DEF("readObject", 1, js_misc_read_object),
    JS_CFUNC_DEF("getOpCodes", 0, js_misc_opcodes),
    JS_CFUNC_DEF("getByteCode", 1, js_misc_get_bytecode),
    JS_CFUNC_MAGIC_DEF("valueType", 1, js_misc_valuetype, VALUE_TYPE),
    JS_CFUNC_MAGIC_DEF("valueTag", 1, js_misc_valuetype, VALUE_TAG),
    JS_CFUNC_MAGIC_DEF("valuePtr", 1, js_misc_valuetype, VALUE_PTR),
    JS_CFUNC_DEF("evalBinary", 1, js_misc_evalbinary),
    JS_CFUNC_MAGIC_DEF("atomToString", 1, js_misc_atom, ATOM_TO_STRING),
    JS_CFUNC_MAGIC_DEF("atomToValue", 1, js_misc_atom, ATOM_TO_VALUE),
    JS_CFUNC_MAGIC_DEF("valueToAtom", 1, js_misc_atom, VALUE_TO_ATOM),
    JS_CFUNC_MAGIC_DEF("getClassID", 1, js_misc_classid, GET_CLASS_ID),
    JS_CFUNC_MAGIC_DEF("getClassName", 1, js_misc_classid, GET_CLASS_NAME),
    JS_CFUNC_MAGIC_DEF("getClassAtom", 1, js_misc_classid, GET_CLASS_ATOM),
    JS_CFUNC_MAGIC_DEF("getClassCount", 1, js_misc_classid, GET_CLASS_COUNT),
    JS_CFUNC_MAGIC_DEF("getClassProto", 1, js_misc_classid, GET_CLASS_PROTO),
    JS_CFUNC_MAGIC_DEF("getClassConstructor", 1, js_misc_classid, GET_CLASS_CONSTRUCTOR),
    JS_CFUNC_MAGIC_DEF("getTypeId", 1, js_misc_type, GET_TYPE_ID),
    JS_CFUNC_MAGIC_DEF("getTypeStr", 1, js_misc_type, GET_TYPE_STR),
    JS_CFUNC_MAGIC_DEF("getTypeName", 1, js_misc_type, GET_TYPE_NAME),
    JS_CFUNC_MAGIC_DEF("rand", 0, js_misc_random, RANDOM_RAND),
    JS_CFUNC_MAGIC_DEF("randi", 0, js_misc_random, RANDOM_RANDI),
    JS_CFUNC_MAGIC_DEF("randf", 0, js_misc_random, RANDOM_RANDF),
    JS_CFUNC_MAGIC_DEF("srand", 1, js_misc_random, RANDOM_SRAND),
    JS_CFUNC_DEF("escape", 1, js_misc_escape),
    JS_CFUNC_DEF("quote", 1, js_misc_quote),
    JS_CFUNC_DEF("error", 0, js_misc_error),
    JS_CFUNC_MAGIC_DEF("isArray", 1, js_misc_is, IS_ARRAY),
    JS_CFUNC_MAGIC_DEF("isBigDecimal", 1, js_misc_is, IS_BIGDECIMAL),
    JS_CFUNC_MAGIC_DEF("isBigFloat", 1, js_misc_is, IS_BIGFLOAT),
    JS_CFUNC_MAGIC_DEF("isBigInt", 1, js_misc_is, IS_BIGINT),
    JS_CFUNC_MAGIC_DEF("isBool", 1, js_misc_is, IS_BOOL),
    JS_CFUNC_MAGIC_DEF("isCFunction", 1, js_misc_is, IS_CFUNCTION),
    JS_CFUNC_MAGIC_DEF("isConstructor", 1, js_misc_is, IS_CONSTRUCTOR),
    JS_CFUNC_MAGIC_DEF("isEmptyString", 1, js_misc_is, IS_EMPTYSTRING),
    JS_CFUNC_MAGIC_DEF("isError", 1, js_misc_is, IS_ERROR),
    JS_CFUNC_MAGIC_DEF("isException", 1, js_misc_is, IS_EXCEPTION),
    JS_CFUNC_MAGIC_DEF("isExtensible", 1, js_misc_is, IS_EXTENSIBLE),
    JS_CFUNC_MAGIC_DEF("isFunction", 1, js_misc_is, IS_FUNCTION),
    JS_CFUNC_MAGIC_DEF("isHTMLDDA", 1, js_misc_is, IS_HTMLDDA),
    JS_CFUNC_MAGIC_DEF("isInstanceOf", 1, js_misc_is, IS_INSTANCEOF),
    JS_CFUNC_MAGIC_DEF("isInteger", 1, js_misc_is, IS_INTEGER),
    JS_CFUNC_MAGIC_DEF("isJobPending", 1, js_misc_is, IS_JOBPENDING),
    JS_CFUNC_MAGIC_DEF("isLiveObject", 1, js_misc_is, IS_LIVEOBJECT),
    JS_CFUNC_MAGIC_DEF("isNull", 1, js_misc_is, IS_NULL),
    JS_CFUNC_MAGIC_DEF("isNumber", 1, js_misc_is, IS_NUMBER),
    JS_CFUNC_MAGIC_DEF("isObject", 1, js_misc_is, IS_OBJECT),
    JS_CFUNC_MAGIC_DEF("isRegisteredClass", 1, js_misc_is, IS_REGISTEREDCLASS),
    JS_CFUNC_MAGIC_DEF("isString", 1, js_misc_is, IS_STRING),
    JS_CFUNC_MAGIC_DEF("isSymbol", 1, js_misc_is, IS_SYMBOL),
    JS_CFUNC_MAGIC_DEF("isUncatchableError", 1, js_misc_is, IS_UNCATCHABLEERROR),
    JS_CFUNC_MAGIC_DEF("isUndefined", 1, js_misc_is, IS_UNDEFINED),
    JS_CFUNC_MAGIC_DEF("isUninitialized", 1, js_misc_is, IS_UNINITIALIZED),
    JS_CFUNC_MAGIC_DEF("isArrayBuffer", 1, js_misc_is, IS_ARRAYBUFFER),

    JS_CONSTANT(JS_EVAL_TYPE_GLOBAL),
    JS_CONSTANT(JS_EVAL_TYPE_MODULE),
    JS_CONSTANT(JS_EVAL_TYPE_DIRECT),
    JS_CONSTANT(JS_EVAL_TYPE_INDIRECT),
    JS_CONSTANT(JS_EVAL_TYPE_MASK),
    JS_CONSTANT(JS_EVAL_FLAG_STRICT),
    JS_CONSTANT(JS_EVAL_FLAG_STRIP),
    JS_CONSTANT(JS_EVAL_FLAG_COMPILE_ONLY),
    JS_CONSTANT(JS_EVAL_FLAG_BACKTRACE_BARRIER),
#ifdef HAVE_FNMATCH
    JS_CONSTANT(FNM_CASEFOLD),
#ifdef FNM_EXTMATCH
    JS_CONSTANT(FNM_EXTMATCH),
#endif
    JS_CONSTANT(FNM_FILE_NAME),
    JS_CONSTANT(FNM_LEADING_DIR),
    JS_CONSTANT(FNM_NOESCAPE),
    JS_CONSTANT(FNM_NOMATCH),
    JS_CONSTANT(FNM_PATHNAME),
    JS_CONSTANT(FNM_PERIOD),
#endif
#ifdef HAVE_GLOB
    JS_CONSTANT(GLOB_ERR),
    JS_CONSTANT(GLOB_MARK),
    JS_CONSTANT(GLOB_NOSORT),
    JS_CONSTANT(GLOB_NOCHECK),
    JS_CONSTANT(GLOB_NOMATCH),
    JS_CONSTANT(GLOB_NOESCAPE),
    // JS_CONSTANT(GLOB_PERIOD),
    JS_CONSTANT(GLOB_ALTDIRFUNC),
    JS_CONSTANT(GLOB_BRACE),
    JS_CONSTANT(GLOB_NOMAGIC),
    JS_CONSTANT(GLOB_TILDE),
    // JS_CONSTANT(GLOB_TILDE_CHECK),
    // JS_CONSTANT(GLOB_ONLYDIR),
    JS_CONSTANT(GLOB_MAGCHAR),
    JS_CONSTANT(GLOB_NOSPACE),
    JS_CONSTANT(GLOB_ABORTED),
#endif
#ifdef HAVE_WORDEXP
    JS_CONSTANT(WRDE_SHOWERR),
    JS_CONSTANT(WRDE_UNDEF),
    JS_CONSTANT(WRDE_BADCHAR),
    JS_CONSTANT(WRDE_BADVAL),
    JS_CONSTANT(WRDE_CMDSUB),
    JS_CONSTANT(WRDE_NOCMD),
    JS_CONSTANT(WRDE_NOSPACE),
    JS_CONSTANT(WRDE_SYNTAX),
#endif
#ifdef HAVE_INOTIFY
    JS_CONSTANT(IN_ACCESS),
    JS_CONSTANT(IN_MODIFY),
    JS_CONSTANT(IN_ATTRIB),
    JS_CONSTANT(IN_CLOSE_WRITE),
    JS_CONSTANT(IN_CLOSE_NOWRITE),
    JS_CONSTANT(IN_CLOSE),
    JS_CONSTANT(IN_OPEN),
    JS_CONSTANT(IN_MOVED_FROM),
    JS_CONSTANT(IN_MOVED_TO),
    JS_CONSTANT(IN_MOVE),
    JS_CONSTANT(IN_CREATE),
    JS_CONSTANT(IN_DELETE),
    JS_CONSTANT(IN_DELETE_SELF),
    JS_CONSTANT(IN_MOVE_SELF),
    JS_CONSTANT(IN_UNMOUNT),
    JS_CONSTANT(IN_Q_OVERFLOW),
    JS_CONSTANT(IN_IGNORED),
    JS_CONSTANT(IN_ONLYDIR),
    JS_CONSTANT(IN_DONT_FOLLOW),
    JS_CONSTANT(IN_EXCL_UNLINK),
    JS_CONSTANT(IN_MASK_ADD),
    JS_CONSTANT(IN_ISDIR),
    JS_CONSTANT(IN_ONESHOT),
    JS_CONSTANT(IN_ALL_EVENTS),
#endif
#ifdef HAVE_TERMIOS_H
    JS_CONSTANT(TIOCSCTTY),
    JS_CONSTANT(TIOCGPGRP),
    JS_CONSTANT(TIOCSPGRP),
    JS_CONSTANT(TIOCGWINSZ),
    JS_CONSTANT(TIOCSWINSZ),
    JS_CONSTANT(TIOCMGET),
    JS_CONSTANT(TIOCMBIS),
    JS_CONSTANT(TIOCMBIC),
    JS_CONSTANT(TIOCMSET),
    JS_CONSTANT(TIOCINQ),
    JS_CONSTANT(TIOCLINUX),
    JS_CONSTANT(TIOCPKT),
    JS_CONSTANT(TIOCSBRK),
    JS_CONSTANT(TIOCCBRK),
#endif
};

static int
js_misc_init(JSContext* ctx, JSModuleDef* m) {

  if(!js_location_class_id)
    js_location_init(ctx, 0);

  vector_init(&js_misc_atexit_functions, ctx);
  atexit(&js_misc_atexit_handler);

  if(m) {
    JS_SetModuleExportList(ctx, m, js_misc_funcs, countof(js_misc_funcs));
    JS_SetModuleExport(ctx, m, "Location", location_ctor);
  }
  return 0;
}

#if defined(JS_SHARED_LIBRARY) && defined(JS_MISC_MODULE)
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_misc
#endif

VISIBLE JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;

  m = JS_NewCModule(ctx, module_name, js_misc_init);
  if(!m)
    return NULL;
  JS_AddModuleExportList(ctx, m, js_misc_funcs, countof(js_misc_funcs));
  JS_AddModuleExport(ctx, m, "Location");
  return m;
}

/**
 * @}
 */
