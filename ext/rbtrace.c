#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ruby.h>
#include <node.h>
#include <intern.h>

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  /* return (uint64_t)tv.tv_sec*1e3 + (uint64_t)tv.tv_usec*1e-3;*/
  return (uint64_t)tv.tv_sec*1e6 + (uint64_t)tv.tv_usec;
}

struct rbtracer_t {
  int id;

  char *query;
  VALUE self;
  VALUE klass;
  ID mid;
};
typedef struct rbtracer_t rbtracer_t;

#define MAX_TRACERS 100
static rbtracer_t tracers[MAX_TRACERS];
static unsigned int num_tracers = 0;

key_t mqi_key, mqo_key;
int mqi_id = -1, mqo_id = -1;

struct event_msg {
  long mtype;
  char buf[56];
};

#define SEND_EVENT(format, ...) do {\
  uint64_t usec = timeofday_usec();\
  if (false) {\
    fprintf(stderr, "%" PRIu64 "," format, usec, __VA_ARGS__);\
    fprintf(stderr, "\n");\
  } else {\
    struct event_msg msg;\
    int ret = -1, n = 0;\
    \
    msg.mtype = 1;\
    snprintf(msg.buf, sizeof(msg.buf), "%" PRIu64 "," format, usec, __VA_ARGS__);\
    \
    for (n=0; n<10 && ret==-1; n++)\
      ret = msgsnd(mqo_id, &msg, sizeof(msg)-sizeof(long), IPC_NOWAIT);\
    if (ret == -1)\
      fprintf(stderr, "msgsnd(): %s\n", strerror(errno));\
  }\
} while (0)

static int in_event_hook = 0;
static bool event_hook_installed = false;

#define MAX_CALLS 4096
static struct {
  bool enabled;
  uint64_t call_times[ MAX_CALLS ];
  int num_calls;
} slow_tracer = { .enabled = false, .num_calls = 0 };

static void
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  // do not re-enter this function
  // after this, must `goto out` instead of `return`
  if (in_event_hook) return;
  in_event_hook++;

  // skip allocators
  if (mid == ID_ALLOCATOR) goto out;

  // normal klass and test for class-level methods
  bool singleton = 0;
  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC(klass)->klass;
    }
    singleton = FL_TEST(klass, FL_SINGLETON);
  }

  // are we watching for any slow methods?
  if (slow_tracer.enabled) {
    uint64_t usec = timeofday_usec(), diff = 0;

    switch (event) {
      case RUBY_EVENT_C_CALL:
      case RUBY_EVENT_CALL:
        if (slow_tracer.num_calls < MAX_CALLS)
          slow_tracer.call_times[ slow_tracer.num_calls++ ] = usec;
        else
          slow_tracer.num_calls++;
        break;

      case RUBY_EVENT_C_RETURN:
      case RUBY_EVENT_RETURN:
        if (slow_tracer.num_calls > 0) {
          if (slow_tracer.num_calls > MAX_CALLS)
            slow_tracer.num_calls--;
          else
            diff = usec - slow_tracer.call_times[ --slow_tracer.num_calls ];
        }
        break;
    }

    if (diff > 250 * 1e3) {
      SEND_EVENT(
        "%s,-1,%" PRIu64 ",%d,%s,%d,%s",
        event == RUBY_EVENT_RETURN ? "slow" : "cslow",
        diff,
        slow_tracer.num_calls,
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );
    }

    goto out;
  }

  // are there specific methods we're waiting for?
  if (num_tracers == 0) goto out;

  int i, n;
  rbtracer_t *tracer = NULL;

  for (i=0, n=0; i<MAX_TRACERS && n<num_tracers; i++) {
    if (tracers[i].query) {
      n++;

      if (tracers[i].mid == mid) {
        if (!tracers[i].klass || tracers[i].klass == klass) {
          if (!tracers[i].self || tracers[i].self == self) {
            tracer = &tracers[i];
          }
        }
      }
    }
  }

  // no matching method tracer found, so bail!
  if (!tracer) goto out;

  switch (event) {
    case RUBY_EVENT_C_CALL:
    case RUBY_EVENT_CALL:
      SEND_EVENT(
        "%s,%d,%s,%d,%s",
        event == RUBY_EVENT_CALL ? "call" : "ccall",
        tracer->id,
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );
      break;

    case RUBY_EVENT_C_RETURN:
    case RUBY_EVENT_RETURN:
      SEND_EVENT(
        "%s,%d",
        event == RUBY_EVENT_RETURN ? "return" : "creturn",
        tracer->id
      );
      break;
  }

out:
  in_event_hook--;
}

static void
event_hook_install()
{
  if (!event_hook_installed) {
    rb_add_event_hook(
      event_hook,
      RUBY_EVENT_CALL   | RUBY_EVENT_C_CALL |
      RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN
    );
    event_hook_installed = true;
  }
}

static void
event_hook_remove()
{
  if (event_hook_installed) {
    rb_remove_event_hook(event_hook);
    event_hook_installed = false;
  }
}

static int
rbtracer_remove(char *query, int id)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (query) {
    for (i=0; i<MAX_TRACERS; i++) {
      if (tracers[i].query) {
        if (0 == strcmp(query, tracers[i].query)) {
          tracer = &tracers[i];
          break;
        }
      }
    }
  } else {
    if (id >= MAX_TRACERS) goto out;
    tracer = &tracers[id];
  }

  if (tracer->query) {
    tracer_id = tracer->id;
    tracer->mid = 0;
    free(tracer->query);
    tracer->query = NULL;

    num_tracers--;
    if (num_tracers == 0)
      event_hook_remove();
  }

out:
  SEND_EVENT(
    "remove,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static void
rbtracer_remove_all()
{
  int i;
  for (i=0; i<MAX_TRACERS; i++) {
    if (tracers[i].query) {
      rbtracer_remove(NULL, i);
    }
  }
}

static int
rbtracer_add(char *query)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (num_tracers >= MAX_TRACERS) goto out;

  for (i=0; i<MAX_TRACERS; i++) {
    if (!tracers[i].query) {
      tracer = &tracers[i];
      tracer_id = i;
      break;
    }
  }
  if (!tracer) goto out;

  char *index;
  VALUE klass = 0, self = 0;
  ID mid;

  if (NULL != (index = rindex(query, '.'))) {
    *index = 0;
    self = rb_eval_string_protect(query, 0);
    *index = '.';

    mid = rb_intern(index+1);
  } else if (NULL != (index = rindex(query, '#'))) {
    *index = 0;
    klass = rb_eval_string_protect(query, 0);
    *index = '#';

    mid = rb_intern(index+1);
  } else {
    mid = rb_intern(query);
  }

  if (!mid) goto out;

  memset(tracer, 0, sizeof(*tracer));

  tracer->id = tracer_id;
  tracer->self = self;
  tracer->klass = klass;
  tracer->mid = mid;
  tracer->query = strdup(query);

  if (num_tracers == 0)
    event_hook_install();

  num_tracers++;

out:
  SEND_EVENT(
    "add,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static void
rbtracer_watch()
{
  if (!slow_tracer.enabled) {
    slow_tracer.num_calls = 0;
    event_hook_install();
    slow_tracer.enabled = true;
  }
}

static void
rbtracer_unwatch()
{
  if (slow_tracer.enabled) {
    event_hook_remove();
    slow_tracer.enabled = false;
  }
}

static VALUE
rbtrace(VALUE self, VALUE query)
{
  Check_Type(query, T_STRING);

  char *str = RSTRING(query)->ptr;
  int tracer_id = -1;

  tracer_id = rbtracer_add(str);
  return tracer_id == -1 ? Qfalse : Qtrue;
}

static VALUE
untrace(VALUE self, VALUE query)
{
  Check_Type(query, T_STRING);

  char *str = RSTRING(query)->ptr;
  int tracer_id = -1;

  tracer_id = rbtracer_remove(str, -1);
  return tracer_id == -1 ? Qfalse : Qtrue;
}

static void
cleanup()
{
  if (mqo_id != -1) {
    msgctl(mqo_id, IPC_RMID, NULL);
    mqo_id = -1;
  }
  if (mqi_id != -1) {
    msgctl(mqi_id, IPC_RMID, NULL);
    mqi_id = -1;
  }
}

static void
cleanup_ruby(VALUE data)
{
  cleanup();
}

static void
sigurg(int signal)
{
  if (mqi_id == -1) return;

  struct event_msg msg;
  char *query = NULL;
  int len = 0;
  int n = 0;

  while (true) {
    int ret = -1;

    for (n=0; n<10 && ret==-1; n++)
      ret = msgrcv(mqi_id, &msg, sizeof(msg)-sizeof(long), 0, IPC_NOWAIT);

    if (ret == -1) {
      break;
    } else {
      len = strlen(msg.buf);
      if (msg.buf[len-1] == '\n')
        msg.buf[len-1] = 0;

      if (0 == strncmp("add,", msg.buf, 4)) {
        query = msg.buf + 4;
        rbtracer_add(query);

      } else if (0 == strncmp("del,", msg.buf, 4)) {
        query = msg.buf + 4;
        rbtracer_remove(query, -1);

      } else if (0 == strncmp("delall", msg.buf, 6)) {
        rbtracer_remove_all();

      } else if (0 == strncmp("watch", msg.buf, 5)) {
        rbtracer_watch();

      } else if (0 == strncmp("unwatch", msg.buf, 7)) {
        rbtracer_unwatch();

      }
    }
  }
}

void
Init_rbtrace()
{
  atexit(cleanup);
  rb_set_end_proc(cleanup_ruby, 0);

  signal(SIGURG, sigurg);

  memset(&tracers, 0, sizeof(tracers));

  mqo_key = (key_t) getpid();
  mqo_id  = msgget(mqo_key, 0666 | IPC_CREAT);
  if (mqo_id == -1)
    rb_sys_fail("msgget");

  mqi_key = (key_t) -getpid();
  mqi_id  = msgget(mqi_key, 0666 | IPC_CREAT);
  if (mqi_id == -1)
    rb_sys_fail("msgget");

  /* struct msqid_ds stat;*/
  /* msgctl(mqo_id, IPC_STAT, &stat);*/
  /* printf("cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);*/

  rb_define_method(rb_cObject, "rbtrace", rbtrace, 1);
  rb_define_method(rb_cObject, "untrace", untrace, 1);
}
