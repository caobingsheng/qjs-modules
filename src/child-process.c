#include "child-process.h"
#include "utils.h"
#include "property-enumeration.h"
#include "debug.h"

#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#ifdef POSIX_SPAWN
#include <spawn.h>
#else
#include <unistd.h>
#endif
#include <sys/wait.h>
#endif

#if(_XOPEN_SOURCE >= 500 && !(_POSIX_C_SOURCE >= 200809L) || _DEFAULT_SOURCE || _BSD_SOURCE)
#define fork() vfork()
#endif

/**
 * \addtogroup child-process
 * @{
 */
static struct list_head child_process_list = LIST_HEAD_INIT(child_process_list);

void
child_process_sigchld(int pid) {
}

ChildProcess*
child_process_get(int pid) {
  struct list_head* el;
  list_for_each(el, &child_process_list) {
    ChildProcess* cp = list_entry(el, ChildProcess, link);
    if(cp->pid == pid)
      return cp;
  }
  return 0;
}

ChildProcess*
child_process_new(JSContext* ctx) {
  ChildProcess* child;
  child = js_mallocz(ctx, sizeof(ChildProcess));
  list_add_tail(&child->link, &child_process_list);
  child->exitcode = -1;
  child->termsig = -1;
  child->stopsig = -1;
  child->pid = -1;
  return child;
}

char**
child_process_environment(JSContext* ctx, JSValueConst object) {
  PropertyEnumeration propenum;
  Vector args;

  if(property_enumeration_init(&propenum, ctx, object, PROPENUM_DEFAULT_FLAGS))
    return 0;

  vector_init(&args, ctx);

  do {
    char* var;
    const char *name, *value;
    size_t namelen, valuelen;

    name = property_enumeration_keystrlen(&propenum, &namelen, ctx);
    value = property_enumeration_valuestrlen(&propenum, &valuelen, ctx);

    var = js_malloc(ctx, namelen + 1 + valuelen + 1);

    memcpy(var, name, namelen);
    var[namelen] = '=';
    memcpy(&var[namelen + 1], value, valuelen);
    var[namelen + 1 + valuelen] = '\0';

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);

    vector_push(&args, var);

  } while(property_enumeration_next(&propenum));

  vector_emplace(&args, sizeof(char*));
  return (char**)args.data;
}

static char*
argv_to_string(char* const* argv, char delim) {
  int i, len;
  char *ptr, *str;

  if(argv == NULL)
    return NULL;

  for(i = 0, len = 0; argv[i]; i++) { len += strlen(argv[i]) + 1; }

  str = ptr = (char*)malloc(len + 1);
  if(str == NULL)
    return NULL;

  for(i = 0; argv[i]; i++) {
    len = strlen(argv[i]);
    memcpy(ptr, argv[i], len);
    ptr += len;
    *ptr++ = delim;
  }
  *ptr = 0;

  return str;
}

int
child_process_spawn(ChildProcess* cp) {

#ifdef _WIN32
  int i;
  intptr_t pid;
  char* args = argv_to_string(cp->args, ' ');
  PROCESS_INFORMATION piProcessInfo;
  STARTUPINFOA siStartInfo;

  SECURITY_ATTRIBUTES saAttr;
  BOOL retval = FALSE;

  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;

  saAttr.lpSecurityDescriptor = NULL;

  ZeroMemory(&piProcessInfo, sizeof(PROCESS_INFORMATION));
  ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));

  siStartInfo.cb = sizeof(STARTUPINFO);
  siStartInfo.hStdError = (HANDLE)_get_osfhandle(cp->child_fds[2]);
  siStartInfo.hStdOutput = (HANDLE)_get_osfhandle(cp->child_fds[1]);
  siStartInfo.hStdInput = (HANDLE)_get_osfhandle(cp->child_fds[0]);
  siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

  retval = CreateProcessA(cp->file, args, &saAttr, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &siStartInfo, &piProcessInfo);

  if(retval == FALSE) {
    int error = GetLastError();
    fprintf(stderr, "CreateProcessA error: %d\n", error);
    pid = -1;
  } else {
    pid = piProcessInfo.dwProcessId;
  }

#elif defined(POSIX_SPAWN)

  int i;
  pid_t pid;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;

  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, 0);

  posix_spawn_file_actions_init(&actions);

  for(i = 0; i <= 2; i++)
    if(cp->child_fds[i] >= 0)
      posix_spawn_file_actions_adddup2(&actions, cp->child_fds[i], i);

  if(posix_spawnp(&pid, cp->file, &actions, &attr, cp->args, cp->env)) {
    fprintf(stderr, "posix_spawnp error: %s\n", strerror(errno));
    return -1;
  }

  return cp->pid = pid;
#else
  int i;
  pid_t pid;

  if((pid = fork()) == 0) {

    if(cp->parent_fds) {
      for(i = 0; i < cp->num_fds; i++) {
        if(cp->parent_fds && cp->parent_fds[i] >= 0)
          close(cp->parent_fds[i]);
      }
    }

    if(cp->child_fds) {
      for(i = 0; i < cp->num_fds; i++) {
        if(cp->child_fds[i] >= 0) {
          if(cp->child_fds[i] != i) {
            dup2(cp->child_fds[i], i);
            close(cp->child_fds[i]);
          }
        }
      }
    }

    if(cp->cwd)
      chdir(cp->cwd);

    setuid(cp->uid);
    setgid(cp->gid);

#ifdef HAVE_EXECVPE
    execvpe(cp->file, cp->args, cp->env ? cp->env : environ);
    perror("execvp()");
#else
    if(cp->env) {
      size_t i;
      for(i = 0; cp->env[i]; i++) putenv(cp->env[i]);
    }
    execvp(cp->file, cp->args);
    perror("execvp()");
#endif
    exit(errno);
  }

  printf("forked proc %d\n", pid);

  if(cp->child_fds) {
    for(i = 0; i < cp->num_fds; i++) {
      if(cp->child_fds[i] >= 0 && cp->child_fds[i] != i)
        close(cp->child_fds[i]);
    }
  }
  return cp->pid = pid;
#endif
}

int
child_process_wait(ChildProcess* cp, int flags) {
#ifdef _WIN32
  DWORD exitcode = 0;
  HANDLE hproc;
  int i, ret;

  hproc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, cp->pid);

  for(;;) {
    ret = WaitForSingleObject(hproc, INFINITE);

    if(ret == WAIT_TIMEOUT)
      continue;
    if(ret == WAIT_FAILED)
      return -1;

    if(ret == WAIT_OBJECT_0) {
      GetExitCodeProcess(hproc, &exitcode);
      CloseHandle(hproc);
      if(exitcode == STILL_ACTIVE)
        return -1;

      cp->exitcode = exitcode;
      return cp->pid;
    }
  }
  return -1;

#elif defined(POSIX_SPAWN)

  return -1;
#else
  int pid, status;

  if((pid = waitpid(cp->pid, &status, flags)) != cp->pid)
    return pid;

  cp->signaled = WIFSIGNALED(status);
  cp->stopped = WIFSTOPPED(status);
#ifdef WIFCONTINUED
  if((cp->continued = WIFCONTINUED(status)))
    cp->stopsig = -1;
#else
  cp->continued = 0;
#endif

  if(WIFEXITED(status))
    cp->exitcode = WEXITSTATUS(status);

  if(WIFSIGNALED(status) || WIFEXITED(status))
    cp->termsig = WTERMSIG(status);
  if(WIFSTOPPED(status))
    cp->stopsig = WSTOPSIG(status);

  return pid;
#endif
}

int
child_process_kill(ChildProcess* cp, int signum) {
#ifdef _WIN32
  if(TerminateProcess(cp->pid, 0))
    return 0;
  return -1;
#else
  int ret;
  int status;
  ret = kill(cp->pid, signum);

  if(ret != -1 && waitpid(cp->pid, &status, WNOHANG) == cp->pid) {
    if(WIFEXITED(status))
      cp->exitcode = WEXITSTATUS(status);

    if(WIFSIGNALED(status))
      cp->termsig = WTERMSIG(status);
  }
  return ret;
#endif
}

void
child_process_free(ChildProcess* cp, JSContext* ctx) {
  list_del(&cp->link);
  if(cp->file)
    js_free(ctx, cp->file);
  if(cp->cwd)
    js_free(ctx, cp->cwd);
  if(cp->args)
    js_strv_free(ctx, cp->args);
  if(cp->env)
    js_strv_free(ctx, cp->env);

  js_free(ctx, cp);
}

void
child_process_free_rt(ChildProcess* cp, JSRuntime* rt) {
  list_del(&cp->link);
  if(cp->file)
    js_free_rt(rt, cp->file);
  if(cp->cwd)
    js_free_rt(rt, cp->cwd);
  if(cp->args)
    js_strv_free_rt(rt, cp->args);
  if(cp->env)
    js_strv_free_rt(rt, cp->env);

  js_free_rt(rt, cp);
}

const char* child_process_signals[32] = {
    0,         "SIGHUP",  "SIGINT",  "SIGQUIT", "SIGILL",    "SIGTRAP",   "SIGABRT",  "SIGBUS",  "SIGFPE",  "SIGKILL", "SIGUSR1",
    "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM",   "SIGSTKFLT", "SIGCHLD",  "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN",
    "SIGTTOU", "SIGURG",  "SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF",   "SIGWINCH", "SIGIO",   "SIGPWR",  "SIGSYS",
};
/**
 * @}
 */
