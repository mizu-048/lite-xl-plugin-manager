#ifdef _WIN32
  #include <direct.h>
  #include <winsock2.h>
  #include <windows.h>
  #include <fileapi.h>
#else
  #include <pthread.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/ioctl.h>
  #include <arpa/inet.h>
  #include <libgen.h>
  #include <termios.h>

  #define MAX_PATH PATH_MAX
#endif

#include <assert.h>
#include <git2.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/file.h>
#include <git2.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#if MBEDTLS_VERSION_MAJOR < 3
  #include <mbedtls/net.h>
#else
  #include <mbedtls/net_sockets.h>
#endif
#ifdef MBEDTLS_DEBUG_C
  #include <mbedtls/debug.h>
#endif

#include <zlib.h>
#include <microtar.h>
#include <zip.h>

#ifdef __APPLE__
  #include <Security/Security.h>
#endif

#define HTTPS_RESPONSE_HEADER_BUFFER_LENGTH 8192


typedef struct {
  #if _WIN32
    HANDLE thread;
    void* (*func)(void*);
    void* data;
  #else
    pthread_t thread;
  #endif
} thread_t;

typedef struct {
  #if _WIN32
    HANDLE mutex;
  #else
    pthread_mutex_t mutex;
  #endif
} mutex_t;

static mutex_t* new_mutex() {
  mutex_t* mutex = malloc(sizeof(mutex_t));
  #if _WIN32
    mutex->mutex = CreateMutex(NULL, FALSE, NULL);
  #else
    pthread_mutex_init(&mutex->mutex, NULL);
  #endif
  return mutex;
}

static void free_mutex(mutex_t* mutex) {
  #if _WIN32
    CloseHandle(mutex->mutex);
  #else
    pthread_mutex_destroy(&mutex->mutex);
  #endif
  free(mutex);
}

static void lock_mutex(mutex_t* mutex) {
  #if _WIN32
    WaitForSingleObject(mutex->mutex, INFINITE);
  #else
    pthread_mutex_lock(&mutex->mutex);
  #endif
}

static void unlock_mutex(mutex_t* mutex) {
  #if _WIN32
    ReleaseMutex(mutex->mutex);
  #else
    pthread_mutex_unlock(&mutex->mutex);
  #endif
}


#if _WIN32
static DWORD windows_thread_callback(void* data) {
  thread_t* thread = data;
  thread->data = thread->func(thread->data);
  return 0;
}
#endif

static thread_t* create_thread(void* (*func)(void*), void* data) {
  thread_t* thread = malloc(sizeof(thread_t));
  #if _WIN32
    thread->func = func;
    thread->data = data;
    thread->thread = CreateThread(NULL, 0, windows_thread_callback, thread, 0, NULL);
  #else
    pthread_create(&thread->thread, NULL, func, data);
  #endif
  return thread;
}

static void* join_thread(thread_t* thread) {
  if (!thread)
    return NULL;
  void* retval;
  #if _WIN32
    WaitForSingleObject(thread->thread, INFINITE);
  #else
    pthread_join(thread->thread, &retval);
  #endif
  free(thread);
  return retval;
}


#if _WIN32
static LPCWSTR lua_toutf16(lua_State* L, const char* str) {
  if (str && str[0] == 0)
    return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (len > 0) {
    LPWSTR output = (LPWSTR) malloc(sizeof(WCHAR) * len);
    if (output) {
      len = MultiByteToWideChar(CP_UTF8, 0, str, -1, output, len);
      if (len > 0) {
        lua_pushlstring(L, (char*)output, len * 2);
        free(output);
        return (LPCWSTR)lua_tostring(L, -1);
      }
      free(output);
    }
  }
  luaL_error(L, "can't convert utf8 string");
  return NULL;
}

static const char* lua_toutf8(lua_State* L, LPCWSTR str) {
  int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
  if (len > 0) {
    char* output = (char *) malloc(sizeof(char) * len);
    if (output) {
      len = WideCharToMultiByte(CP_UTF8, 0, str, -1, output, len, NULL, NULL);
      if (len) {
        lua_pushlstring(L, output, len - 1);
        free(output);
        return lua_tostring(L, -1);
      }
      free(output);
    }
  }
  luaL_error(L, "can't convert utf16 string");
  return NULL;
}

static const int luaL_win32_error(lua_State* L, DWORD error_id, const char* message, ...) {
  va_list va;
  va_start(va, message);
  lua_pushvfstring(L, message, va);
  va_end(va);
  wchar_t message_buffer[2048];
  size_t size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, error_id, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), message_buffer, 2048, NULL);
  lua_pushliteral(L, ": ");
  lua_toutf8(L, message_buffer);
  lua_concat(L, 3);
  return lua_error(L);
}
#endif

static FILE* lua_fopen(lua_State* L, const char* path, const char* mode) {
  #ifdef _WIN32
    FILE* file = _wfopen(lua_toutf16(L, path), lua_toutf16(L, mode));
    lua_pop(L, 2);
    return file;
  #else
    return fopen(path, mode);
  #endif
}

static char hex_digits[] = "0123456789abcdef";
static void lua_pushhexstring(lua_State* L, const unsigned char* buffer, size_t length) {
  char hex_buffer[length * 2 + 1];
  for (size_t i = 0; i < length; ++i) {
    hex_buffer[i*2+0] = hex_digits[buffer[i] >> 4];
    hex_buffer[i*2+1] = hex_digits[buffer[i] & 0xF];
  }
  lua_pushlstring(L, hex_buffer, length * 2);
}

static int lpm_hash(lua_State* L) {
  size_t len;
  const char* data = luaL_checklstring(L, 1, &len);
  const char* type = luaL_optstring(L, 2, "string");
  static const int digest_length = 32;
  unsigned char buffer[digest_length];
  mbedtls_sha256_context hash_ctx;
  mbedtls_sha256_init(&hash_ctx);
  mbedtls_sha256_starts(&hash_ctx, 0);
  if (strcmp(type, "file") == 0) {
    FILE* file = lua_fopen(L, data, "rb");
    if (!file) {
      mbedtls_sha256_free(&hash_ctx);
      return luaL_error(L, "can't open %s", data);
    }
    while (1) {
      unsigned char chunk[4096];
      size_t bytes = fread(chunk, 1, sizeof(chunk), file);
      mbedtls_sha256_update(&hash_ctx, chunk, bytes);
      if (bytes < sizeof(chunk))
        break;
    }
    fclose(file);
  } else {
    mbedtls_sha256_update(&hash_ctx, data, len);
  }
  mbedtls_sha256_finish(&hash_ctx, buffer);
  mbedtls_sha256_free(&hash_ctx);
  lua_pushhexstring(L, buffer, digest_length);
  return 1;
}

static int lpm_tcflush(lua_State* L) {
  int stream = luaL_checkinteger(L, 1);
  #ifndef _WIN32
    if (isatty(stream))
      tcflush(stream, TCIOFLUSH);
  #endif
  return 0;
}

static int lpm_tcwidth(lua_State* L) {
  int stream = luaL_checkinteger(L, 1);
  #ifndef _WIN32
    if (isatty(stream)) {
      struct winsize ws={0};
      ioctl(stream, TIOCGWINSZ, &ws);
      lua_pushinteger(L, ws.ws_col);
      return 1;
    }
  #else
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int columns, rows;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
      lua_pushinteger(L, csbi.srWindow.Right - csbi.srWindow.Left + 1);
      return 1;
    }
  #endif
  return 0;
}

static int lpm_symlink(lua_State* L) {
  #ifndef _WIN32
    if (symlink(luaL_checkstring(L, 1), luaL_checkstring(L, 2)))
      return luaL_error(L, "can't create symlink %s: %s", luaL_checkstring(L, 2), strerror(errno));
    return 0;
  #else
    return luaL_error(L, "can't create symbolic link %s: your operating system sucks", luaL_checkstring(L, 2));
  #endif
}

static int lpm_chmod(lua_State* L) {
  #ifdef _WIN32
    if (_wchmod(lua_toutf16(L, luaL_checkstring(L, 1)), luaL_checkinteger(L, 2)))
  #else
    if (chmod(luaL_checkstring(L, 1), luaL_checkinteger(L, 2)))
  #endif
      return luaL_error(L, "can't chmod %s: %s", luaL_checkstring(L, 1), strerror(errno));
  return 0;
}

static int lpm_ls(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  int i = 1;
#ifdef _WIN32
  lua_settop(L, 1);
  lua_pushstring(L, path[0] == 0 || strpbrk(&path[strlen(path) - 1], "\\/") != NULL ? "*" : "\\*");
  lua_concat(L, 2);
  path = lua_tostring(L, -1);

  WIN32_FIND_DATAW fd;
  HANDLE find_handle = FindFirstFileExW(lua_toutf16(L, path), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
  if (find_handle == INVALID_HANDLE_VALUE)
    return luaL_win32_error(L, GetLastError(), "can't ls %s", path);
  lua_newtable(L);

  do {
    const char* filename = lua_toutf8(L, fd.cFileName);
    if (strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
      lua_rawseti(L, -2, i++);
    } else
      lua_pop(L, 1);
  } while (FindNextFileW(find_handle, &fd));
  int err = GetLastError();
  FindClose(find_handle);
  if (err != ERROR_NO_MORE_FILES)
    return luaL_win32_error(L, err, "can't ls %s", path);
#else
  DIR *dir = opendir(path);
  if (!dir)
    return luaL_error(L, "can't ls %s: %s", path, strerror(errno));
  lua_newtable(L);
  struct dirent *entry;
  while ( (entry = readdir(dir)) ) {
    if (strcmp(entry->d_name, "." ) == 0) { continue; }
    if (strcmp(entry->d_name, "..") == 0) { continue; }
    lua_pushstring(L, entry->d_name);
    lua_rawseti(L, -2, i++);
  }
  closedir(dir);
#endif
  return 1;
}

static int lpm_rmdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
  if (!RemoveDirectoryW(lua_toutf16(L, path)))
    return luaL_win32_error(L, GetLastError(), "can't rmdir %s", path);
#else
  if (remove(path))
    return luaL_error(L, "can't rmdir %s: %s", path, strerror(errno));
#endif
  return 0;
}

static int lpm_mkdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
  int err = _wmkdir(lua_toutf16(L, path));
#else
  int err = mkdir(path, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
#endif
  if (err < 0)
    return luaL_error(L, "can't mkdir %s: %s", path, strerror(errno));
  return 0;
}

static int lpm_stat(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
#ifdef _WIN32
  wchar_t full_path[MAX_PATH];
  struct _stat s;
  LPCWSTR wpath = lua_toutf16(L, path);
  int err = _wstat(wpath, &s);
  const char *abs_path = !err && _wfullpath(full_path, wpath, MAX_PATH) ? lua_toutf8(L, (LPCWSTR)full_path) : NULL;
#else
  char full_path[MAX_PATH];
  struct stat s;
  int err = lstat(path, &s);
  const char *abs_path = NULL;
  if (!err) {
    if (S_ISLNK(s.st_mode)) {
      char folder_path[MAX_PATH];
      strcpy(folder_path, path);
      abs_path = realpath(dirname(folder_path), full_path);
      if (abs_path) {
        strcat(full_path, "/");
        strcpy(folder_path, path);
        strcat(full_path, basename(folder_path));
      }
    } else
      abs_path = realpath(path, full_path);
  }
#endif
  if (err || !abs_path) {
    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  lua_newtable(L);
  lua_pushstring(L, abs_path); lua_setfield(L, -2, "abs_path");
  lua_pushvalue(L, 1); lua_setfield(L, -2, "path");

#if __linux__
  if (S_ISLNK(s.st_mode)) {
    char buffer[PATH_MAX];
    ssize_t len = readlink(path, buffer, sizeof(buffer));
    if (len < 0)
      return 0;
    lua_pushlstring(L, buffer, len);
  } else
    lua_pushnil(L);
  lua_setfield(L, -2, "symlink");
  if (S_ISLNK(s.st_mode))
    err = stat(path, &s);
  if (err)
    return 1;
#endif
  lua_pushinteger(L, s.st_mtime); lua_setfield(L, -2, "modified");
  lua_pushinteger(L, s.st_size); lua_setfield(L, -2, "size");
  lua_pushinteger(L, s.st_mode); lua_setfield(L, -2, "mode");
  if (S_ISREG(s.st_mode)) {
    lua_pushstring(L, "file");
  } else if (S_ISDIR(s.st_mode)) {
    lua_pushstring(L, "dir");
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "type");
  return 1;
}
/** END STOLEN LITE CODE **/

static const char* git_error_last_string() {
  const git_error* last_error = git_error_last();
  return last_error->message;
}

static int git_get_id(git_oid* commit_id, git_repository* repository, const char* name) {
  int length = strlen(name);
  int is_hex = 1;
  for (int i = 0; is_hex && i < length; ++i)
    is_hex = isxdigit(name[i]);
  if (!is_hex || length < 3)
    return git_reference_name_to_id(commit_id, repository, name);
  if (length < GIT_OID_SHA1_SIZE*2) {
    if (length % 2 != 0)
      return -1;
    git_commit* commit;
    git_oid oid = {0};
    for (int i = 0; i < length/2; ++i)
      oid.id[i] |= (name[i] - '0') << ((i % 2) * 4);
    int ret = git_commit_lookup_prefix(&commit, repository, &oid, length/2);
    if (ret)
      return ret;
    git_oid_cpy(commit_id, git_commit_id(commit));
    git_commit_free(commit);
    return 0;
  }
  return git_oid_fromstr(commit_id, name);
}

static git_repository* luaL_checkgitrepo(lua_State* L, int index) {
  const char* path = luaL_checkstring(L, index);
  git_repository* repository;
  if (git_repository_open(&repository, path))
    return (void*)(long long)luaL_error(L, "git open error: %s", git_error_last_string());
  return repository;
}


static git_commit* git_retrieve_commit(git_repository* repository, const char* commit_name) {
  git_oid commit_id;
  git_commit* commit;
  if (git_get_id(&commit_id, repository, commit_name) || git_commit_lookup(&commit, repository, &commit_id))
    return NULL;
  return commit;
}

// We move this out of main, because this is a significantly expensive function,
// and we don't need to call it every time we run lpm.
static int git_initialized = 0;
static int git_cert_type = 0;
static char git_cert_path[MAX_PATH];
static void git_init() {
  if (!git_initialized) {
    git_libgit2_init();
    if (git_cert_type)
      git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, git_cert_type == 2 ? git_cert_path : NULL, git_cert_type == 1 ? git_cert_path : NULL);
    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, ".");
    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_GLOBAL, ".");
    git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, ".");
    git_initialized = 1;
  }
}


static int lpm_reset(lua_State* L) {
  git_init();
  git_repository* repository = luaL_checkgitrepo(L, 1);
  const char* commit_name = luaL_checkstring(L, 2);
  const char* type = luaL_checkstring(L, 3);
  git_commit* commit = git_retrieve_commit(repository, commit_name);
  if (!commit) {
    git_repository_free(repository);
    return luaL_error(L, "git retrieve commit error: %s", git_error_last_string());
  }
  git_reset_t reset_type = GIT_RESET_SOFT;
  if (strcmp(type, "mixed") == 0)
    reset_type = GIT_RESET_MIXED;
  else if (strcmp(type, "hard") == 0)
    reset_type = GIT_RESET_HARD;
  int result = git_reset(repository, (git_object*)commit, reset_type, NULL);
  git_commit_free(commit);
  git_repository_free(repository);
  if (result)
    return luaL_error(L, "git reset error: %s", git_error_last_string());
  return 0;
}


static int lpm_init(lua_State* L) {
  git_init();
  const char* path = luaL_checkstring(L, 1);
  const char* url = luaL_checkstring(L, 2);
  git_repository* repository;
  if (git_repository_init(&repository, path, 0))
    return luaL_error(L, "git init error: %s", git_error_last_string());
  git_remote* remote;
  if (git_remote_create(&remote, repository, "origin", url)) {
    git_repository_free(repository);
    return luaL_error(L, "git remote add error: %s", git_error_last_string());
  }
  git_remote_free(remote);
  git_repository_free(repository);
  return 0;
}

static int no_verify_ssl, has_setup_ssl, print_trace;
static mbedtls_x509_crt x509_certificate;
static mbedtls_entropy_context entropy_context;
static mbedtls_ctr_drbg_context drbg_context;
static mbedtls_ssl_config ssl_config;
static mbedtls_ssl_context ssl_context;

static int lpm_git_transport_certificate_check_cb(struct git_cert *cert, int valid, const char *host, void *payload) {
  return 0; // If no_verify_ssl is enabled, basically always return 0 when this is set as callback.
}


typedef struct {
  git_repository* repository;
  lua_State* L;
  char refspec[512];
  int depth;
  int threaded;
  int callback_function;
  git_transfer_progress progress;
  int complete;
  int error;
  char data[512];
  thread_t* thread;
} fetch_context_t;

static int lpm_fetch_callback(lua_State* L, const git_transfer_progress *stats) {
  lua_pushinteger(L, stats->received_bytes);
  lua_pushinteger(L, stats->total_objects);
  lua_pushinteger(L, stats->indexed_objects);
  lua_pushinteger(L, stats->received_objects);
  lua_pushinteger(L, stats->local_objects);
  lua_pushinteger(L, stats->total_deltas);
  lua_pushinteger(L, stats->indexed_deltas);
  return lua_pcall(L, 7, 0, 0);
}

static int lpm_git_transfer_progress_cb(const git_transfer_progress *stats, void *payload) {
  fetch_context_t* context = (fetch_context_t*)payload;
  if (!context->threaded) {
    if (context->callback_function) {
      lua_rawgeti(context->L, LUA_REGISTRYINDEX, context->callback_function);
      lpm_fetch_callback(context->L, stats);
    }
  } else
    context->progress = *stats;
}

static int lua_is_main_thread(lua_State* L) {
  int is_main = lua_pushthread(L);
  lua_pop(L, 1);
  return is_main;
}

static void* lpm_fetch_thread(void* ctx) {
  fflush(stderr);
  git_remote* remote;
  fetch_context_t* context = (fetch_context_t*)ctx;
  fflush(stderr);
  int error = git_remote_lookup(&remote, context->repository, "origin");
  if (error && !context->error) {
    snprintf(context->data, sizeof(context->data), "git remote fetch error: %s", git_error_last_string());
    error = context->error;
    return NULL;
  }
  git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
  fetch_opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_ALL;
  fetch_opts.callbacks.payload = context;
  #if (LIBGIT2_VER_MAJOR == 1 && LIBGIT2_VER_MINOR >= 7) || LIBGIT2_VER_MAJOR > 1
  fetch_opts.depth = context->depth;
  #endif
  if (no_verify_ssl)
    fetch_opts.callbacks.certificate_check = lpm_git_transport_certificate_check_cb;
  fetch_opts.callbacks.transfer_progress = lpm_git_transfer_progress_cb;
  char* strings[] = { context->refspec };
  git_strarray array = { strings, 1 };

  error = git_remote_connect(remote, GIT_DIRECTION_FETCH, &fetch_opts.callbacks, NULL, NULL) ||
    git_remote_download(remote, context->refspec[0] ? &array : NULL, &fetch_opts) ||
    git_remote_update_tips(remote, &fetch_opts.callbacks, fetch_opts.update_fetchhead, fetch_opts.download_tags, NULL);
  if (!error && !context->error) {
    git_buf branch_name = {0};
    if (!git_remote_default_branch(&branch_name, remote)) {
      strncpy(context->data, branch_name.ptr, sizeof(context->data));
      git_buf_dispose(&branch_name);
    }
  }
  git_remote_disconnect(remote);
  git_remote_free(remote);
  if (error && !context->error) {
    snprintf(context->data, sizeof(context->data), "git remote fetch error: %s", git_error_last_string());
    context->error = error;
  }
  context->complete = 1;
  return NULL;
}


static int lpm_fetchk(lua_State* L, int status, lua_KContext ctx) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, (int)ctx);
  fetch_context_t* context = lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (context->threaded && context->callback_function) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, context->callback_function);
    context->error = lpm_fetch_callback(L, &context->progress);
    if (context->error)
      strncpy(context->data, lua_tostring(L, -1), sizeof(context->data));
  }
  if (context->complete || context->error) {
    join_thread(context->thread);
    git_repository_free(context->repository);
    if (context->data[0] == 0)
      lua_pushnil(L);
    else
      lua_pushstring(L, context->data);
    if (context->callback_function)
      luaL_unref(L, LUA_REGISTRYINDEX, context->callback_function);
    luaL_unref(L, LUA_REGISTRYINDEX, (int)ctx);
    if (context->error)
      lua_error(L);
    return 1;
  }
  assert(context->threaded);
  return lua_yieldk(L, 0, (lua_KContext)ctx, lpm_fetchk);
}


static int lpm_fetch(lua_State* L) {
  git_init();
  fetch_context_t* context = lua_newuserdata(L, sizeof(fetch_context_t));
  memset(context, 0, sizeof(fetch_context_t));
  context->repository = luaL_checkgitrepo(L, 1);
  const char* refspec = luaL_optstring(L, 3, NULL);
  context->depth = lua_toboolean(L, 4) ? GIT_FETCH_DEPTH_FULL : 1;
  context->L = L;
  context->threaded = !lua_is_main_thread(L);
  if (refspec)
    strncpy(context->refspec, refspec, sizeof(context->refspec));
  if (lua_type(L, 2) == LUA_TFUNCTION) {
    lua_pushvalue(L, 2);
    context->callback_function = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  int ctx = luaL_ref(L, LUA_REGISTRYINDEX);
  if (lua_is_main_thread(L)) {
    lpm_fetch_thread(context);
    lpm_fetchk(L, 0, ctx);
    return 0;
  } else {
    context->thread = create_thread(lpm_fetch_thread, context);
    return lua_yieldk(L, 0, (lua_KContext)ctx, lpm_fetchk);
  }
}


static void lpm_tls_debug(void *ctx, int level, const char *file, int line, const char *str) {
  fprintf(stderr, "%s:%04d: |%d| %s", file, line, level, str);
  fflush(stderr);
}

static void lpm_libgit2_debug(git_trace_level_t level, const char *msg) {
  fprintf(stderr, "[libgit2]: %s\n", msg);
  fflush(stderr);
}

static int lpm_trace(lua_State* L) {
  print_trace = lua_toboolean(L, 1) ? 1 : 0;
  return 0;
}


static int luaL_mbedtls_error(lua_State* L, int code, const char* str, ...) {
  char vsnbuffer[1024];
  char mbed_buffer[128];
  mbedtls_strerror(code, mbed_buffer, sizeof(mbed_buffer));
  va_list va;
  va_start(va, str);
      vsnprintf(vsnbuffer, sizeof(vsnbuffer), str, va);
  va_end(va);
  return luaL_error(L, "%s: %s", vsnbuffer, mbed_buffer);
}


static int lpm_certs(lua_State* L) {
  const char* type = luaL_checkstring(L, 1);
  int status;
  if (has_setup_ssl) {
    mbedtls_ssl_config_free(&ssl_config);
    mbedtls_ctr_drbg_free(&drbg_context);
    mbedtls_entropy_free(&entropy_context);
    mbedtls_x509_crt_free(&x509_certificate);
  }
  mbedtls_x509_crt_init(&x509_certificate);
  mbedtls_entropy_init(&entropy_context);
  mbedtls_ctr_drbg_init(&drbg_context);
  if ((status = mbedtls_ctr_drbg_seed(&drbg_context, mbedtls_entropy_func, &entropy_context, NULL, 0)) != 0)
    return luaL_mbedtls_error(L, status, "failed to setup mbedtls_x509");
  mbedtls_ssl_config_init(&ssl_config);
  status = mbedtls_ssl_config_defaults(&ssl_config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (status)
    return luaL_mbedtls_error(L, status, "can't set ssl_config defaults");
  mbedtls_ssl_conf_max_version(&ssl_config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_min_version(&ssl_config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
  mbedtls_ssl_conf_authmode(&ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_rng(&ssl_config, mbedtls_ctr_drbg_random, &drbg_context);
  mbedtls_ssl_conf_read_timeout(&ssl_config, 5000);
  #if defined(MBEDTLS_DEBUG_C)
  if (print_trace) {
    mbedtls_debug_set_threshold(5);
    mbedtls_ssl_conf_dbg(&ssl_config, lpm_tls_debug, NULL);
    git_init();
    git_trace_set(GIT_TRACE_TRACE, lpm_libgit2_debug);
  }
  #endif
  has_setup_ssl = 1;
  if (strcmp(type, "noverify") == 0) {
    no_verify_ssl = 1;
    mbedtls_ssl_conf_authmode(&ssl_config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    if (print_trace) {
      fprintf(stderr, "[ssl] SSL verify set to optional.\n");
      fflush(stderr);
    }
  } else {
    const char* path = luaL_checkstring(L, 2);
    if (strcmp(type, "dir") == 0) {
      git_cert_type = 1;
      if (git_initialized)
        git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, NULL, path);
      strncpy(git_cert_path, path, MAX_PATH);
      if (print_trace) {
        fprintf(stderr, "[ssl] SSL directory set to %s.\n", git_cert_path);
        fflush(stderr);
      }
      status = mbedtls_x509_crt_parse_path(&x509_certificate, path);
      if (status < 0)
        return luaL_mbedtls_error(L, status, "mbedtls_x509_crt_parse_path failed to parse all CA certificates in %s", path);
      if (status > 0 && print_trace) {
        fprintf(stderr, "[ssl] mbedtls_x509_crt_parse_path on %s failed to parse %d certificates, but still succeeded.\n", path, status);
        fflush(stderr);
      }
      mbedtls_ssl_conf_ca_chain(&ssl_config, &x509_certificate, NULL);
    } else {
      if (strcmp(type, "system") == 0) {
        #if _WIN32
          FILE* file = lua_fopen(L, path, "wb");
          if (!file)
            return luaL_error(L, "can't open cert store %s for writing: %s", path, strerror(errno));
          HCERTSTORE hSystemStore = CertOpenSystemStore(0, TEXT("ROOT"));
          if (!hSystemStore) {
            fclose(file);
            return luaL_error(L, "error getting system certificate store");
          }
          PCCERT_CONTEXT pCertContext = NULL;
          while (1) {
            pCertContext = CertEnumCertificatesInStore(hSystemStore, pCertContext);
            if (!pCertContext)
              break;
            BYTE keyUsage[2];
            if (pCertContext->dwCertEncodingType & X509_ASN_ENCODING && (CertGetIntendedKeyUsage(pCertContext->dwCertEncodingType, pCertContext->pCertInfo, keyUsage, sizeof(keyUsage)) && (keyUsage[0] & CERT_KEY_CERT_SIGN_KEY_USAGE))) {
              DWORD size = 0;
              CryptBinaryToString(pCertContext->pbCertEncoded, pCertContext->cbCertEncoded, CRYPT_STRING_BASE64HEADER, NULL, &size);
              char* buffer = malloc(size);
              CryptBinaryToString(pCertContext->pbCertEncoded, pCertContext->cbCertEncoded, CRYPT_STRING_BASE64HEADER, buffer, &size);
              fwrite(buffer, sizeof(char), size, file);
              free(buffer);
            }
          }
          fclose(file);
          CertCloseStore(hSystemStore, 0);
          if (print_trace) {
            fprintf(stderr, "[ssl] SSL file pulled from system store and written to %s.\n", path);
            fflush(stderr);
          }
        #elif __APPLE__ // https://developer.apple.com/forums/thread/691009; see also curl's mac version
          return luaL_error(L, "can't use system on mac yet");
        #else
          return luaL_error(L, "can't use system certificates except on windows or mac");
        #endif
      }
      git_cert_type = 2;
      if (git_initialized)
        git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, path, NULL);
      strncpy(git_cert_path, path, MAX_PATH);
      if (print_trace) {
        fprintf(stderr, "[ssl] SSL file set to %s.\n", git_cert_path);
        fflush(stderr);
      }
      status = mbedtls_x509_crt_parse_file(&x509_certificate, path);
      if (status < 0)
        return luaL_mbedtls_error(L, status, "mbedtls_x509_crt_parse_file failed to parse CA certificate %s", path);
      if (status > 0 && print_trace) {
        fprintf(stderr, "[ssl] mbedtls_x509_crt_parse_file on %s failed to parse %d certificates, but still still succeeded.\n", path, status);
        fflush(stderr);
      }
      mbedtls_ssl_conf_ca_chain(&ssl_config, &x509_certificate, NULL);
      if (print_trace) {
        fprintf(stderr, "[ssl] SSL file set to %s.\n", path);
        fflush(stderr);
      }
    }
  }
  return 0;
}


static int mkdirp(char* path, int len) {
  for (int i = 0; i < len; ++i) {
    if (path[i] == '/' && i > 0) {
      path[i] = 0;
      #ifndef _WIN32
        if (mkdir(path, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) && errno != EEXIST)
      #else
        if (mkdir(path) && errno != EEXIST)
      #endif
        return -1;
      path[i] = '/';
    }
  }
  return 0;
}

#define FA_RDONLY       0x01            // FILE_ATTRIBUTE_READONLY
#define FA_DIREC        0x10            // FILE_ATTRIBUTE_DIRECTORY

static int lpm_extract(lua_State* L) {
  const char* src = luaL_checkstring(L, 1);
  const char* dst = luaL_checkstring(L, 2);

  if (strstr(src, ".zip")) {
    int zip_error_code;
    zip_t* archive = zip_open(src, ZIP_RDONLY, &zip_error_code);

    if (!archive) {
      zip_error_t zip_error;
      zip_error_init_with_code(&zip_error, zip_error_code);
      lua_pushfstring(L, "can't open zip archive %s: %s", src, zip_error_strerror(&zip_error));
      zip_error_fini(&zip_error);
      return lua_error(L);
    }

    zip_int64_t entries = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < entries; ++i) {
      zip_file_t* zip_file = zip_fopen_index(archive, i, 0);
      const char* zip_name = zip_get_name(archive, i, ZIP_FL_ENC_GUESS);

      if (!zip_file) {
        lua_pushfstring(L, "can't read zip archive file %s: %s", zip_name, zip_strerror(archive));
        zip_close(archive);
        return lua_error(L);
      }

      char target[MAX_PATH];
      int target_length = snprintf(target, sizeof(target), "%s/%s", dst, zip_name);

      if (mkdirp(target, target_length)) {
        zip_fclose(zip_file);
        zip_close(archive);
        return luaL_error(L, "can't extract zip archive file %s, can't create directory %s: %s", src, target, strerror(errno));
      }

      if (target[target_length-1] != '/') {
        FILE* file = lua_fopen(L, target, "wb");
        if (!file) {
          zip_fclose(zip_file);
          zip_close(archive);
          return luaL_error(L, "can't write file %s: %s", target, strerror(errno));
        }

        mode_t m = S_IRUSR | S_IRGRP | S_IROTH;
        zip_uint8_t os;
        zip_uint32_t attr;

        zip_file_get_external_attributes(archive, i, 0, &os, &attr);
        if (os == ZIP_OPSYS_DOS) {
          if (0 == (attr & FA_RDONLY))
              m |= S_IWUSR | S_IWGRP | S_IWOTH;
          if (attr & FA_DIREC)
              m = (S_IFDIR | (m & ~S_IFMT)) | S_IXUSR | S_IXGRP | S_IXOTH;
        } else {
          m = (attr >> 16);
        }

        if (chmod(target, m)) {
          zip_fclose(zip_file);
          zip_close(archive);
          return luaL_error(L, "can't chmod file %s: %s", target, strerror(errno));
        }

        while (1) {
          char buffer[8192];
          zip_int64_t length = zip_fread(zip_file, buffer, sizeof(buffer));

          if (length == -1) {
            lua_pushfstring(L, "can't read zip archive file  %s: %s", zip_name, zip_file_strerror(zip_file));
            zip_fclose(zip_file);
            zip_close(archive);
            return lua_error(L);
          }

          if (length == 0) break;
          fwrite(buffer, sizeof(char), length, file);
        }

        fclose(file);
      }
      zip_fclose(zip_file);
    }
    zip_close(archive);
  }

  else {
    char actual_src[PATH_MAX];

    if (strstr(src, ".gz") || strstr(src, ".tgz")) {
      gzFile gzfile = gzopen(src, "rb");

      if (!gzfile)
        return luaL_error(L, "can't open tar.gz archive %s: %s", src, strerror(errno));

      char buffer[8192];
      int len = strlen(src) - 3;

      if (strstr(src, ".tar"))
        strncpy(actual_src, src, len < PATH_MAX ? len : PATH_MAX);
      else if (strstr(src, ".tgz")) {
        strncpy(actual_src, src, len < PATH_MAX ? len : PATH_MAX);
        strcat(actual_src, "tar");
        len = strlen(src);
      }
      else{
        strcpy(actual_src, dst);
      }

      actual_src[len] = 0;
      FILE* file = lua_fopen(L, actual_src, "wb");

      if (!file) {
        gzclose(gzfile);
        return luaL_error(L, "can't open %s for writing: %s", actual_src, strerror(errno));
      }

      while (1) {
        int length = gzread(gzfile, buffer, sizeof(buffer));
        if (length == 0)
          break;
        fwrite(buffer, sizeof(char), length, file);
      }

      char error[128];
      error[0] = 0;
      if (!gzeof(gzfile)) {
        int error_number;
        strncpy(error, gzerror(gzfile, &error_number), sizeof(error));
        error[sizeof(error)-1] = 0;
      }

      fclose(file);
      gzclose(gzfile);

      if (error[0])
        return luaL_error(L, "can't unzip gzip archive %s: %s", src, error);

    } else
      strcpy(actual_src, src);

    if (strstr(src, ".tar") || strstr(src, ".tgz")) {
      /* It's incredibly slow to do it this way, probably because of all the seeking.
      For now, just gunzip the whole file at once, and then untar it.
      tar.read = gzip_read;
      tar.seek = gzip_seek;
      tar.close = gzip_close;*/

      mtar_t tar = {0};
      int err;
      if ((err = mtar_open(&tar, actual_src, "r")))
        return luaL_error(L, "can't open tar archive %s: %s", src, mtar_strerror(err));

      mtar_header_t h;
      mtar_header_t before_h;
      mtar_header_t allways_h;
      int has_ext_before = 0;
      int has_ext_allways = 0;

      mtar_clear_header(&before_h);
      mtar_clear_header(&allways_h);

      while ((mtar_read_header(&tar, &h)) != MTAR_ENULLRECORD ) {
        if (h.type == MTAR_TREG) {

          if (has_ext_before) {
            mtar_update_header(&h, &before_h);
            has_ext_before = 0;
            mtar_clear_header(&before_h);
          }
          if (has_ext_allways)
            mtar_update_header(&h, &allways_h);

          char target[MAX_PATH];
          int target_length = snprintf(target, sizeof(target), "%s/%s", dst, h.name);

          if (mkdirp(target, target_length)) {
            mtar_close(&tar);
            return luaL_error(L, "can't extract tar archive file %s, can't create directory %s: %s", src, target, strerror(errno));
          }

          FILE* file = fopen(target, "wb");
          if (!file) {
            mtar_close(&tar);
            return luaL_error(L, "can't extract tar archive file %s, can't create file %s: %s", src, target, strerror(errno));
          }

          if (chmod(target, h.mode))
            return luaL_error(L, "can't extract tar archive file %s, can't chmod file %s: %s", src, target, strerror(errno));

          char buffer[8192];
          int remaining = h.size;
          while (remaining > 0) {
            int read_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

            int err = mtar_read_data(&tar, buffer, read_size);
            if (err != MTAR_ESUCCESS) {
              fclose(file);
              mtar_close(&tar);
              return luaL_error(L, "can't read file %s: %s", target, mtar_strerror(err));
            }

            fwrite(buffer, sizeof(char), read_size, file);
            remaining -= read_size;
          }

          fclose(file);
        }

        else if (h.type == MTAR_TEHR || h.type == MTAR_TEHRA) {
          mtar_header_t *h_to_change;
          if (h.type == MTAR_TEHR)
            h_to_change = &before_h;
          else
            h_to_change = &allways_h;

          char buffer[4096] = {0};
          char current_read[8192] = {0}; // If a line is more than 8192 char long, will not work!
          char last_read[4096] = {0};
          int remaining = h.size;

          has_ext_before = 1;

          while (remaining > 0) {
            int read_size = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            remaining -= read_size;

            if (mtar_read_data(&tar, buffer, read_size) != MTAR_ESUCCESS) {
              mtar_close(&tar);
              return luaL_error(L, "Error while reading extended: %s", strerror(errno));
            }

            strcpy(current_read, last_read);
            current_read[strlen(last_read)] = '\0';
            strcat(current_read, buffer);
            current_read[strlen(last_read) + read_size] = '\0';

            char *n_line_ptr = NULL;
            char **l_line_ptr = NULL;
            char *line = strtok_r(current_read, "\n", &n_line_ptr);

            while (line != NULL) {
              char *in_line_ptr = NULL;
              strtok_r(line, " ", &in_line_ptr);
              char *header_key = strtok_r(NULL, "=", &in_line_ptr);
              char *header_val = strtok_r(NULL, "=", &in_line_ptr);

              if (!strcmp(header_key, "path"))      strcpy(h_to_change->name, header_val);
              if (!strcmp(header_key, "linkpath"))  strcpy(h_to_change->linkname, header_val);
                // possibility to add more later

              l_line_ptr = &n_line_ptr;
              line = strtok_r(NULL, "\n", &n_line_ptr);
            }

            if (current_read[strlen(last_read) + read_size - 1] != '\n')
              strcpy(last_read, strtok_r(current_read, "\n", l_line_ptr));
            else
              memset(last_read, 0, strlen(last_read));
          }
        }

        else if (h.type == MTAR_TGFP) {
          has_ext_before = 1;
          int read_size = before_h.size < sizeof(before_h.name) ? before_h.size : sizeof(before_h.name);

          if (mtar_read_data(&tar, before_h.name, read_size) != MTAR_ESUCCESS) {
            mtar_close(&tar);
            return luaL_error(L, "Error while reading GNU extended: %s", strerror(errno));
          }
        }

        else if (h.type == MTAR_TGLP) {
          has_ext_before = 1;
          int read_size = before_h.size < sizeof(before_h.linkname) ? before_h.size : sizeof(before_h.linkname);

          if (mtar_read_data(&tar, before_h.linkname, read_size) != MTAR_ESUCCESS) {
            mtar_close(&tar);
            return luaL_error(L, "Error while reading GNU extended: %s", strerror(errno));
          }
        }

        mtar_next(&tar);
      }
      mtar_close(&tar);
      if (strstr(src, ".gz") || strstr(src, ".tgz"))
        unlink(actual_src);
    }
  }
  return 0;
}


static int strncicmp(const char* a, const char* b, int n) {
  for (int i = 0; i < n; ++i) {
    if (a[i] == 0 && b[i] != 0) return -1;
    if (a[i] != 0 && b[i] == 0) return 1;
    int lowera = tolower(a[i]), lowerb = tolower(b[i]);
    if (lowera == lowerb) continue;
    if (lowera < lowerb) return -1;
    return 1;
  }
  return 0;
}

static const char* strnstr_local(const char* haystack, const char* needle, int n) {
  int len = strlen(needle);
  for (int i = 0; i <= n - len; ++i) {
    if (strncmp(&haystack[i], needle, len) == 0)
      return &haystack[i];
  }
  return NULL;
}

static const char* get_header(const char* buffer, const char* header, int* len) {
  const char* line_end = strstr(buffer, "\r\n");
  const char* header_end = strstr(buffer, "\r\n\r\n");
  int header_len = strlen(header);
  while (line_end && line_end < header_end) {
    if (strncicmp(line_end + 2, header, header_len) == 0) {
      const char* offset = line_end + header_len + 3;
      while (*offset == ' ') { ++offset; }
      const char* end = strstr(offset, "\r\n");
      if (len)
        *len = end - offset;
      return offset;
    }
    line_end = strstr(line_end + 2, "\r\n");
  }
  return NULL;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

typedef enum {
  STATE_CONNECT,
  STATE_HANDSHAKE,
  STATE_SEND,
  STATE_RECV_HEADER,
  STATE_RECV_BODY
} get_state_e;

typedef struct {
  get_state_e state;
  int s;
  int is_ssl;
  mbedtls_ssl_context ssl;
  mbedtls_net_context net;
  int lua_buffer;
  FILE* file;
  char address[1024];
  int error_code;
  char error[256];
  char hostname[256];
  char rest[256];
  int callback_function;

  char buffer[HTTPS_RESPONSE_HEADER_BUFFER_LENGTH];
  int buffer_length;

  int content_length;
  int chunk_length;
  int chunked;
  int chunk_written;
  int total_downloaded;
} get_context_t;


static int lpm_socket_write(get_context_t* context, int len) {
  return context->is_ssl ? mbedtls_ssl_write(&context->ssl, context->buffer, len) : write(context->s, context->buffer, len);
}

static int lpm_socket_read(get_context_t* context, int len) {
  if (len == -1)
    len = sizeof(context->buffer) - context->buffer_length;
  if (len == 0)
    return len;
  len = context->is_ssl ? mbedtls_ssl_read(&context->ssl, &context->buffer[context->buffer_length], len) : read(context->s, &context->buffer[context->buffer_length], len);
  if (len > 0)
    context->buffer_length += len;
  return len;
}


static int lpm_get_error(get_context_t* context, int error_code, const char* str, ...) {
  if (error_code) {
    context->error_code = error_code;
    char mbed_buffer[256];
    mbedtls_strerror(error_code, mbed_buffer, sizeof(mbed_buffer));
    int error_len = context->is_ssl ? strlen(mbed_buffer) : strlen(strerror(error_code));
    va_list va;
    int offset = 0;
    va_start(va, str);
      offset = vsnprintf(context->buffer, sizeof(context->buffer), str, va);
    va_end(va);
    if (offset < sizeof(context->buffer) - 2) {
      strcat(context->buffer, ": ");
      if (offset < sizeof(context->buffer) - error_len - 2)
        strcat(context->buffer, context->is_ssl ? mbed_buffer : strerror(error_code));
    }
  }
  return error_code;
}

static int lpm_set_error(get_context_t* context, const char* str, ...) {
  va_list va;
  int offset = 0;
  va_start(va, str);
    offset = vsnprintf(context->error, sizeof(context->error), str, va);
  va_end(va);
  context->error_code = -1;
  return offset;
}

static int lpm_getk(lua_State* L, int status, lua_KContext ctx) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, ctx);
  get_context_t* context = (get_context_t*)lua_touserdata(L, -1);
  lua_pop(L,1);
  switch (context->state) {
    case STATE_HANDSHAKE: {
      int status = mbedtls_ssl_handshake(&context->ssl);
      if (status == MBEDTLS_ERR_SSL_WANT_READ || status == MBEDTLS_ERR_SSL_WANT_WRITE)
        return lua_yieldk(L, 0, ctx, lpm_getk);
      if (
        lpm_get_error(context, status, "can't handshake") ||
        lpm_get_error(context, mbedtls_ssl_get_verify_result(&context->ssl), "can't verify result")
      )
        goto cleanup;
      context->state = STATE_SEND;
    }
    case STATE_SEND: {
      context->buffer_length = snprintf(context->buffer, sizeof(context->buffer), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", context->rest, context->hostname);
      int length = lpm_socket_write(context, context->buffer_length);
      if (length < context->buffer_length && lpm_get_error(context, length, "can't write to socket"))
        goto cleanup;
      context->state = STATE_RECV_HEADER;
      context->buffer_length = 0;
      context->buffer[0] = 0;
    }
    case STATE_RECV_HEADER: {
      const char* header_end;
      while (1) {
        header_end = strstr(context->buffer, "\r\n\r\n");
        if (!header_end && context->buffer_length >= sizeof(context->buffer) - 1 && lpm_set_error(context, "response header buffer length exceeded"))
          goto cleanup;
        if (!header_end) {
          int length = lpm_socket_read(context, -1);
          if (length < 0 && lpm_get_error(context, length, "can't read from socket"))
            goto cleanup;
          if (length == 0)
            return lua_yieldk(L, 0, ctx, lpm_getk);
        } else {
          header_end += 4;
          const char* protocol_end = strnstr_local(context->buffer, " ", context->buffer_length);
          int code = atoi(protocol_end + 1);
          if (code != 200) {
            if (code >= 301 && code <= 303) {
              const char* location = get_header(context->buffer, "location", &context->buffer_length);
              if (location) {
                lua_pushnil(L);
                lua_newtable(L);
                lua_pushlstring(L, location, context->buffer_length);
                lua_setfield(L, -2, "location");
              } else
                lpm_set_error(context, "received invalid %d-response", code);
            } else
              lpm_set_error(context, "received non 200-response of %d", code);
            goto report;
          }
          const char* transfer_encoding = get_header(context->buffer, "transfer-encoding", NULL);
          context->chunked = transfer_encoding && strncmp(transfer_encoding, "chunked", 7) == 0 ? 1 : 0;
          const char* content_length_value = get_header(context->buffer, "content-length", NULL);
          context->content_length = content_length_value ? atoi(content_length_value) : -1;
          context->buffer_length -= (header_end - context->buffer);
          if (context->buffer_length > 0)
            memmove(context->buffer, header_end, context->buffer_length);
          context->chunk_length = !context->chunked && context->content_length == -1 ? INT_MAX : context->content_length;
          context->state = STATE_RECV_BODY;
          break;
        }
      }
    }
    case STATE_RECV_BODY: {
      while (1) {
        // If we have an unknown amount of chunk bytes to be fetched, determine the size of the next chunk.
        while (context->chunk_length == -1) {
          char* newline = (char*)strnstr_local(context->buffer, "\r\n", context->buffer_length);
          if (newline) {
            *newline = '\0';
            if ((sscanf(context->buffer, "%x", &context->chunk_length) != 1 && lpm_set_error(context, "error retrieving chunk length")))
              goto cleanup;
            else if (context->chunk_length == 0)
              goto finish;
            context->buffer_length -= (newline + 2 - context->buffer);
            if (context->buffer_length > 0)
              memmove(context->buffer, newline + 2, context->buffer_length);
          } else if (context->buffer_length >= sizeof(context->buffer) && lpm_set_error(context, "can't find chunk length")) {
            goto cleanup;
          } else {
            int length = lpm_socket_read(context, -1);
            if ((length <= 0 || (context->is_ssl && length == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)) && lpm_get_error(context, length, "error retrieving full repsonse"))
              goto cleanup;
            if (length == 0)
              return lua_yieldk(L, 0, ctx, lpm_getk);
          }
        }
        if (context->buffer_length > 0) {
          int to_write = imin(context->chunk_length - context->chunk_written, context->buffer_length);
          if (to_write > 0) {
            context->total_downloaded += to_write;
            context->chunk_written += to_write;
            if (context->callback_function) {
              lua_rawgeti(L, LUA_REGISTRYINDEX, context->callback_function);
              lua_pushinteger(L, context->total_downloaded);
              if (context->content_length == -1)
                lua_pushnil(L);
              else
                lua_pushinteger(L, context->content_length);
              lua_call(L, 2, 0);
            }
            if (context->file)
              fwrite(context->buffer, sizeof(char), to_write, context->file);
            else {
              lua_rawgeti(L, LUA_REGISTRYINDEX, context->lua_buffer);
              lua_pushlstring(L, context->buffer, to_write);
              lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
              lua_pop(L, 1);
            }
            context->buffer_length -= to_write;
            if (context->buffer_length > 0)
              memmove(context->buffer, &context->buffer[to_write], context->buffer_length);
          }
          if (context->chunk_written == context->chunk_length) {
            if (!context->chunked)
              goto finish;
            if (context->buffer_length >= 2) {
              if (!strnstr_local(context->buffer, "\r\n", 2) && lpm_set_error(context, "invalid end to chunk"))
                goto cleanup;
              memmove(context->buffer, &context->buffer[2], context->buffer_length - 2);
              context->buffer_length -= 2;
              context->chunk_length = -1;
            }
          }
        }
        if (context->chunk_length > 0) {
          int length = lpm_socket_read(context, imin(sizeof(context->buffer) - context->buffer_length, context->chunk_length - context->chunk_written + (context->chunked ? 2 : 0)));
          if ((!context->is_ssl && length == 0) || (context->is_ssl && context->content_length == -1 && length == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY))
            goto finish;
          if (length < 0 && lpm_get_error(context, length, "error retrieving full chunk"))
            goto cleanup;
          if (length == 0)
            return lua_yieldk(L, 0, ctx, lpm_getk);
        }
      }
    }
  }
  finish:
  if (context->file) {
    lua_pushnil(L);
    lua_newtable(L);
  } else {
    lua_rawgeti(L, LUA_REGISTRYINDEX, context->lua_buffer);
    size_t len = lua_rawlen(L, -1);
    luaL_Buffer b;
    int table = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= len; ++i) {
      lua_rawgeti(L, table, i);
      size_t str_len;
      const char* str = lua_tolstring(L, -1, &str_len);
      lua_pop(L, 1);
      luaL_addlstring(&b, str, str_len);
    }
    lua_pop(L, 1);
    luaL_pushresult(&b);
    lua_newtable(L);
  }
  if (context->content_length != -1 && context->total_downloaded != context->content_length && lpm_set_error(context, "error retrieving full response"))
    goto cleanup;
  report:
  if (context->callback_function && !context->error_code) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, context->callback_function);
    lua_pushboolean(L, 1);
    lua_call(L, 1, 0);
  }
  cleanup:
  if (context->is_ssl) {
    mbedtls_ssl_free(&context->ssl);
    mbedtls_net_free(&context->net);
  } else {
    close(context->s);
  }
  if (context->callback_function)
    luaL_unref(L, LUA_REGISTRYINDEX, context->callback_function);
  if (context->file)
    fclose(context->file);
  else
    luaL_unref(L, LUA_REGISTRYINDEX, context->lua_buffer);
  if (context->error_code)
    return luaL_error(L, "%s", context->error);
  return 2;
}

static int lpm_get(lua_State* L) {
  get_context_t* context = lua_newuserdata(L, sizeof(get_context_t));
  memset(context, 0, sizeof(get_context_t));
  int threaded = !lua_is_main_thread(L);

  const char* protocol = luaL_checkstring(L, 1);
  strncpy(context->hostname, luaL_checkstring(L, 2), sizeof(context->hostname));
  strncpy(context->rest, luaL_checkstring(L, 4), sizeof(context->rest));
  const char* path = luaL_optstring(L, 5, NULL);
  if (path) {
    if ((context->file = lua_fopen(L, path, "wb")) == NULL)
      return luaL_error(L, "can't open file %s: %s", path, strerror(errno));
  } else {
    lua_newtable(L);
    context->lua_buffer = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  if (lua_type(L, 6) == LUA_TFUNCTION) {
    lua_pushvalue(L, 6);
    context->callback_function = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  context->state = STATE_CONNECT;

  if (strcmp(protocol, "https") == 0) {
    const char* port = lua_tostring(L, 3);
    // https://gist.github.com/Barakat/675c041fd94435b270a25b5881987a30
    mbedtls_ssl_init(&context->ssl);
    mbedtls_net_init(&context->net);
    if (threaded)
      mbedtls_net_set_nonblock(&context->net);
    else
      mbedtls_net_set_block(&context->net);
    mbedtls_ssl_set_bio(&context->ssl, &context->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    if (
      lpm_get_error(context, mbedtls_ssl_setup(&context->ssl, &ssl_config), "can't set up ssl") ||
      lpm_get_error(context, mbedtls_net_connect(&context->net, context->hostname, port, MBEDTLS_NET_PROTO_TCP), "can't set hostname") ||
      lpm_get_error(context, mbedtls_ssl_set_hostname(&context->ssl, context->hostname), "can't set hostname")
    ) {
      mbedtls_ssl_free(&context->ssl);
      mbedtls_net_free(&context->net);
      return luaL_error(L, "%s", context->error);
    }
    context->is_ssl = 1;
    context->state = STATE_HANDSHAKE;
  } else {
    int port = luaL_checkinteger(L, 3);
    struct hostent *host = gethostbyname(context->hostname);
    struct sockaddr_in dest_addr = {0};
    if (!host)
      return luaL_error(L, "can't resolve hostname %s", context->hostname);
    context->s = socket(AF_INET, SOCK_STREAM, 0);
    if (threaded)
      fcntl(context->s, F_SETFL, fcntl(context->s, F_GETFL, 0) | O_NONBLOCK);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    dest_addr.sin_addr.s_addr = *(long*)(host->h_addr);
    const char* ip = inet_ntoa(dest_addr.sin_addr);
    if (connect(context->s, (struct sockaddr *) &dest_addr, sizeof(struct sockaddr)) == -1 ) {
      close(context->s);
      return luaL_error(L, "can't connect to host %s [%s] on port %d", context->hostname, ip, port);
    }
    context->state = STATE_SEND;
  }
  if (!threaded)
    return lpm_getk(L, 0, luaL_ref(L, LUA_REGISTRYINDEX));
  return lua_yieldk(L, 0, luaL_ref(L, LUA_REGISTRYINDEX), lpm_getk);
}


static int lpm_chdir(lua_State* L) {
  #ifdef _WIN32
    if (_wchdir(lua_toutf16(L, luaL_checkstring(L, 1))))
  #else
    if (chdir(luaL_checkstring(L, 1)))
  #endif
      return luaL_error(L, "error chdiring: %s", strerror(errno));
  return 0;
}

static int lpm_pwd(lua_State* L) {
  #ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (!_wgetcwd(buffer, sizeof(buffer)))
      return luaL_error(L, "error getcwd: %s", strerror(errno));
    lua_toutf8(L, buffer);
  #else
    char buffer[MAX_PATH];
    if (!getcwd(buffer, sizeof(buffer)))
      return luaL_error(L, "error getcwd: %s", strerror(errno));
    lua_pushstring(L, buffer);
  #endif
  return 1;
}

static int lpm_flock(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  int error_handler = lua_type(L, 3) == LUA_TFUNCTION ? 3 : 0;
  int warning_handler = lua_type(L, 4) == LUA_TFUNCTION ? 4 : 0;
  #ifdef _WIN32
    HANDLE file = CreateFileW(lua_toutf16(L, path), FILE_SHARE_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (!file || file == INVALID_HANDLE_VALUE)
      return luaL_win32_error(L, GetLastError(), "can't open for flock %s", path);
    OVERLAPPED overlapped = {0};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 0, 1, &overlapped)) {
      if (GetLastError() == ERROR_IO_PENDING && warning_handler) {
        lua_pushvalue(L, warning_handler);
        lua_pcall(L, 0, 0, 0);
      }
      if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 0, 1, &overlapped)) {
        CloseHandle(file);
        return luaL_win32_error(L, GetLastError(), "can't flock %s", path);
      }
    }
  #else
    int fd = open(path, 0);
    if (fd == -1)
      return luaL_error(L, "can't flock %s: %s", path, strerror(errno));
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
      if (errno == EWOULDBLOCK && warning_handler) {
        lua_pushvalue(L, warning_handler);
        lua_pcall(L, 0, 0, 0);
      }
      if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return luaL_error(L, "can't acquire exclusive lock on %s: %s", strerror(errno));
      }
    }
  #endif
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 1);
  int err = lua_pcall(L, 1, 0, error_handler);
  #ifdef _WIN32
    UnlockFile(file, 0, 0, 1, 0);
    CloseHandle(file);
  #else
    flock(fd, LOCK_UN);
    close(fd);
  #endif
  if (err) {
    lua_pushboolean(L, 1);
    return 1;
  }
  return 0;
}

static double get_time() {
   #if _WIN32 // Fuck I hate windows jesus chrsit.
    LARGE_INTEGER LoggedTime, Frequency;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&LoggedTime);
    return LoggedTime.QuadPart / (double)Frequency.QuadPart;
  #else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
  #endif
}

static int lpm_time(lua_State* L) {
  lua_pushnumber(L, get_time());
  return 1;
}

static const luaL_Reg system_lib[] = {
  { "ls",        lpm_ls    },    // Returns an array of files.
  { "stat",      lpm_stat  },    // Returns info about a single file.
  { "mkdir",     lpm_mkdir },    // Makes a directory.
  { "rmdir",     lpm_rmdir },    // Removes a directory.
  { "hash",      lpm_hash  },    // Returns a hex sha256 hash.
  { "tcflush",   lpm_tcflush },  // Flushes an terminal stream.
  { "tcwidth",   lpm_tcwidth },  // Gets the terminal width in columns.
  { "symlink",   lpm_symlink },  // Creates a symlink.
  { "chmod",     lpm_chmod },    // Chmod's a file.
  { "init",      lpm_init },     // Initializes a git repository with the specified remote.
  { "fetch",     lpm_fetch },    // Updates a git repository with the specified remote.
  { "reset",     lpm_reset },    // Updates a git repository to the specified commit/hash/branch.
  { "get",       lpm_get },      // HTTP(s) GET request.
  { "extract",   lpm_extract },  // Extracts .tar.gz, and .zip files.
  { "trace",     lpm_trace },    // Sets trace bit.
  { "certs",     lpm_certs },    // Sets the SSL certificate chain folder/file.
  { "chdir",     lpm_chdir },    // Changes directory. Only use for --post actions.
  { "pwd",       lpm_pwd },      // Gets existing directory. Only use for --post actions.
  { "flock",     lpm_flock },    // Locks a file.
  { "time",      lpm_time },     // Get high-precision system time.
  { NULL,        NULL }
};

#ifndef ARCH_PROCESSOR
  #if defined(__x86_64__) || defined(_M_AMD64) || defined(__MINGW64__)
    #define ARCH_PROCESSOR "x86_64"
  #elif defined(__i386__) || defined(_M_IX86) || defined(__MINGW32__)
    #define ARCH_PROCESSOR "x86"
  #elif defined(__aarch64__) || defined(_M_ARM64) || defined (_M_ARM64EC)
    #define ARCH_PROCESSOR "aarch64"
  #elif defined(__arm__) || defined(_M_ARM)
    #define ARCH_PROCESSOR "arm"
  #elif defined(__riscv_xlen) && __riscv_xlen == 32
    #define ARCH_PROCESSOR "riscv32"
  #elif defined(__riscv_xlen) && __riscv_xlen == 64
    #define ARCH_PROCESSOR "riscv64"
  #else
    #error "Please define -DARCH_PROCESSOR."
  #endif
#endif
#ifndef ARCH_PLATFORM
  #if _WIN32
    #define ARCH_PLATFORM "windows"
  #elif __ANDROID__
    #define ARCH_PLATFORM "android"
  #elif __linux__
    #define ARCH_PLATFORM "linux"
  #elif __APPLE__
    #define ARCH_PLATFORM "darwin"
  #else
    #error "Please define -DARCH_PLATFORM."
  #endif
#endif
#ifndef LITE_ARCH_TUPLE
  #define LITE_ARCH_TUPLE ARCH_PROCESSOR "-" ARCH_PLATFORM
#endif


#ifndef LPM_VERSION
  #define LPM_VERSION "unknown"
#endif
#ifndef LPM_DEFAULT_REPOSITORY
  #define LPM_DEFAULT_REPOSITORY "https://github.com/lite-xl/lite-xl-plugin-manager.git:latest"
#endif
// If this is defined as empty string, we disable self-upgrading.
#ifndef LPM_DEFAULT_RELEASE
  #if _WIN32
    #define LPM_DEFAULT_RELEASE "https://github.com/lite-xl/lite-xl-plugin-manager/releases/download/%r/lpm." LITE_ARCH_TUPLE ".exe"
  #else
    #define LPM_DEFAULT_RELEASE "https://github.com/lite-xl/lite-xl-plugin-manager/releases/download/%r/lpm." LITE_ARCH_TUPLE
  #endif
#endif

#ifdef LPM_STATIC
  extern const char lpm_luac[];
  extern unsigned int lpm_luac_len;
#endif

int main(int argc, char* argv[]) {
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  luaL_newlib(L, system_lib);
  lua_setglobal(L, "system");
  lua_newtable(L);
  for (int i = 0; i < argc; ++i) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i+1);
  }
  lua_setglobal(L, "ARGV");
  lua_pushliteral(L, LPM_VERSION);
  lua_setglobal(L, "VERSION");
  lua_pushliteral(L, ARCH_PLATFORM);
  lua_setglobal(L, "PLATFORM");
  #if _WIN32
    DWORD handles[] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    int setVirtualProcessing = 0;
    for (int i = 0; i < 2; ++i) {
      DWORD mode = 0;
      if (GetConsoleMode(GetStdHandle(handles[i]), &mode)) {
        if (SetConsoleMode(GetStdHandle(handles[i]), mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
          setVirtualProcessing = 1;
      }
    }
    // This will fail with mintty, see here: https://github.com/mintty/mintty/issues/482
    lua_pushboolean(L, setVirtualProcessing || isatty(fileno(stdout)));
  #else
    lua_pushboolean(L, isatty(fileno(stdout)));
  #endif
  lua_setglobal(L, "TTY");
  #if _WIN32
    lua_pushliteral(L, "\\");
  #else
    lua_pushliteral(L, "/");
  #endif
  lua_setglobal(L, "PATHSEP");
  #if _WIN32
    wchar_t tmpdir[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, tmpdir);
    tmpdir[length - 1] = 0;
    lua_toutf8(L, tmpdir);
  #else
    lua_pushstring(L, getenv("TMPDIR") ? getenv("TMPDIR") : P_tmpdir);
  #endif
  lua_setglobal(L, "SYSTMPDIR");

  #if _WIN32
    wchar_t selfpath[MAX_PATH] = {0};
    if (GetModuleFileNameW(0, selfpath, MAX_PATH - 1))
      lua_toutf8(L, selfpath);
    else
      lua_pushnil(L);
  #else
    char selfpath[MAX_PATH] = {0};
    int length = readlink("/proc/self/exe", selfpath, MAX_PATH);
    if (length > 0)
      lua_pushlstring(L, selfpath, length);
    else
      lua_pushnil(L);
  #endif
  lua_setglobal(L, "EXEFILE");

  lua_pushliteral(L, LITE_ARCH_TUPLE);
  lua_setglobal(L, "DEFAULT_ARCH");
  lua_pushliteral(L, LPM_DEFAULT_REPOSITORY);
  lua_setglobal(L, "DEFAULT_REPO_URL");
  lua_pushliteral(L, LPM_DEFAULT_RELEASE);
  lua_setglobal(L, "DEFAULT_RELEASE_URL");
  #ifndef LPM_STATIC
  if (luaL_loadfile(L, "src/lpm.lua") || lua_pcall(L, 0, 1, 0)) {
  #else
  if (luaL_loadbuffer(L, lpm_luac, lpm_luac_len, "lpm.lua") || lua_pcall(L, 0, 1, 0)) {
  #endif
    fprintf(stderr, "internal error when starting the application: %s\n", lua_tostring(L, -1));
    return -1;
  }
  int status = lua_tointeger(L, -1);
  lua_close(L);
  if (git_initialized)
    git_libgit2_shutdown();
  return status;
}
