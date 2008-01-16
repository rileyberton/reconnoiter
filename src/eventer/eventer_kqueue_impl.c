/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 */

#include "noit_defines.h"
#include "eventer/eventer.h"
#include "utils/noit_atomic.h"
#include "utils/noit_skiplist.h"
#include "utils/noit_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <pthread.h>
#include <assert.h>

static struct timeval __max_sleeptime = { 0, 200000 }; /* 200ms */
static int maxfds;
static struct {
  eventer_t e;
  pthread_t executor;
  noit_spinlock_t lock;
} *master_fds = NULL;

typedef enum { EV_OWNED, EV_ALREADY_OWNED } ev_lock_state_t;

static ev_lock_state_t
acquire_master_fd(int fd) {
  if(noit_spinlock_trylock(&master_fds[fd].lock)) {
    master_fds[fd].executor = pthread_self();
    return EV_OWNED;
  }
  if(pthread_equal(master_fds[fd].executor, pthread_self())) {
    return EV_ALREADY_OWNED;
  }
  noit_spinlock_lock(&master_fds[fd].lock);
  master_fds[fd].executor = pthread_self();
  return EV_OWNED;
}
static void
release_master_fd(int fd, ev_lock_state_t as) {
  if(as == EV_OWNED) {
    memset(&master_fds[fd].executor, 0, sizeof(master_fds[fd].executor));
    noit_spinlock_unlock(&master_fds[fd].lock);
  }
}

static pthread_t master_thread;
static int kqueue_fd = -1;
typedef struct kqueue_setup {
  struct kevent *__ke_vec;
  unsigned int __ke_vec_a;
  unsigned int __ke_vec_used;
} *kqs_t;

static pthread_mutex_t kqs_lock;
static pthread_mutex_t te_lock;
static kqs_t master_kqs = NULL;
static pthread_key_t kqueue_setup_key;
static noit_skiplist *timed_events = NULL;
#define KQUEUE_DECL kqs_t kqs
#define KQUEUE_SETUP kqs = (kqs_t) pthread_getspecific(kqueue_setup_key)
#define ke_vec kqs->__ke_vec
#define ke_vec_a kqs->__ke_vec_a
#define ke_vec_used kqs->__ke_vec_used

static void kqs_init(kqs_t kqs) {
  enum { initial_alloc = 64 };
  ke_vec_a = initial_alloc;
  ke_vec = (struct kevent *) malloc(ke_vec_a * sizeof (struct kevent));
}
static void
ke_change (register int const ident,
           register int const filter,
           register int const flags,
           register void *const udata) {
  register struct kevent *kep;
  KQUEUE_DECL;
  KQUEUE_SETUP;
  if(!kqs) kqs = master_kqs;

  if(kqs == master_kqs) pthread_mutex_lock(&kqs_lock);
  if (!ke_vec_a) {
    kqs_init(kqs);
  }
  else if (ke_vec_used == ke_vec_a) {
    ke_vec_a <<= 1;
    ke_vec = (struct kevent *) realloc(ke_vec,
                                       ke_vec_a * sizeof (struct kevent));
  }
  kep = &ke_vec[ke_vec_used++];

  EV_SET(kep, ident, filter, flags, 0, 0, udata);
  if(kqs == master_kqs) pthread_mutex_unlock(&kqs_lock);
}

static int eventer_kqueue_impl_init() {
  struct rlimit rlim;
  master_thread = pthread_self();
  kqueue_fd = kqueue();
  if(kqueue_fd == -1) {
    return -1;
  }
  pthread_mutex_init(&kqs_lock, NULL);
  pthread_mutex_init(&te_lock, NULL);
  pthread_key_create(&kqueue_setup_key, NULL);
  master_kqs = calloc(1, sizeof(*master_kqs));
  kqs_init(master_kqs);
  getrlimit(RLIMIT_NOFILE, &rlim);
  maxfds = rlim.rlim_cur;
  master_fds = calloc(maxfds, sizeof(*master_fds));
  timed_events = calloc(1, sizeof(*timed_events));
  noit_skiplist_init(timed_events);
  noit_skiplist_set_compare(timed_events,
                            eventer_timecompare, eventer_timecompare);
  noit_skiplist_add_index(timed_events,
                          noit_compare_voidptr, noit_compare_voidptr);
  return 0;
}
static int eventer_kqueue_impl_propset(const char *key, const char *value) {
  return -1;
}
static void eventer_kqueue_impl_add(eventer_t e) {
  assert(e->mask);
  ev_lock_state_t lockstate;

  /* Timed events are simple */
  if(e->mask == EVENTER_TIMER) {
    pthread_mutex_lock(&te_lock);
    noit_skiplist_insert(timed_events, e);
    pthread_mutex_unlock(&te_lock);
    return;
  }

  /* file descriptor event */
  lockstate = acquire_master_fd(e->fd);
  master_fds[e->fd].e = e;
  if(e->mask & (EVENTER_READ | EVENTER_EXCEPTION))
    ke_change(e->fd, EVFILT_READ, EV_ADD | EV_ENABLE, e);
  if(e->mask & (EVENTER_WRITE))
    ke_change(e->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, e);
  release_master_fd(e->fd, lockstate);
}
static void eventer_kqueue_impl_remove(eventer_t e) {
  if(e->mask & (EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION)) {
    ev_lock_state_t lockstate;
    lockstate = acquire_master_fd(e->fd);
    if(e == master_fds[e->fd].e) {
      master_fds[e->fd].e = NULL;
      if(e->mask & (EVENTER_READ | EVENTER_EXCEPTION))
        ke_change(e->fd, EVFILT_READ, EV_DELETE | EV_DISABLE, e);
      if(e->mask & (EVENTER_WRITE))
        ke_change(e->fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, e);
    }
    release_master_fd(e->fd, lockstate);
  }
  else if(e->mask & EVENTER_TIMER) {
    pthread_mutex_lock(&te_lock);
    noit_skiplist_remove_compare(timed_events, e, NULL, noit_compare_voidptr);
    pthread_mutex_unlock(&te_lock);
  }
  else {
    abort();
  }
}
static void eventer_kqueue_impl_update(eventer_t e) {
  if(e->mask & EVENTER_TIMER) {
    pthread_mutex_lock(&te_lock);
    noit_skiplist_remove_compare(timed_events, e, NULL, noit_compare_voidptr);
    noit_skiplist_insert(timed_events, e);
    pthread_mutex_unlock(&te_lock);
    return;
  }
  ke_change(e->fd, EVFILT_READ, EV_DELETE | EV_DISABLE, e);
  ke_change(e->fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, e);
  if(e->mask & (EVENTER_READ | EVENTER_EXCEPTION))
    ke_change(e->fd, EVFILT_READ, EV_DELETE | EV_DISABLE, e);
  if(e->mask & (EVENTER_WRITE))
    ke_change(e->fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, e);
}
static eventer_t eventer_kqueue_impl_remove_fd(int fd) {
  eventer_t eiq = NULL;
  ev_lock_state_t lockstate;
  if(master_fds[fd].e) {
    lockstate = acquire_master_fd(fd);
    eiq = master_fds[fd].e;
    master_fds[fd].e = NULL;
    if(eiq->mask & (EVENTER_READ | EVENTER_EXCEPTION))
      ke_change(fd, EVFILT_READ, EV_DELETE | EV_DISABLE, eiq);
    if(eiq->mask & (EVENTER_WRITE))
      ke_change(fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, eiq);
    release_master_fd(fd, lockstate);
  }
  return eiq;
}
static void eventer_kqueue_impl_loop() {
  int is_master_thread = 0;
  pthread_t self;
  KQUEUE_DECL;
  KQUEUE_SETUP;

  self = pthread_self();
  if(pthread_equal(self, master_thread)) is_master_thread = 1;

  if(!kqs) {
    kqs = calloc(1, sizeof(*kqs));
    kqs_init(kqs);
  }
  pthread_setspecific(kqueue_setup_key, kqs);
  while(1) {
    struct timeval __now, __sleeptime;
    struct timespec __kqueue_sleeptime;
    int fd_cnt = 0;
    int max_timed_events_to_process;
    int newmask;

    __sleeptime = __max_sleeptime;

    /* Handle timed events...
     * we could be multithreaded, so if we pop forever we could starve
     * ourselves. */
    max_timed_events_to_process = timed_events->size;
    while(max_timed_events_to_process-- > 0) {
      eventer_t timed_event;

      gettimeofday(&__now, NULL);

      pthread_mutex_lock(&te_lock);
      /* Peek at our next timed event, if should fire, pop it.
       * otherwise we noop and NULL it out to break the loop. */
      timed_event = noit_skiplist_peek(timed_events);
      if(timed_event) {
        if(compare_timeval(timed_event->whence, __now) < 0) {
          timed_event = noit_skiplist_pop(timed_events, NULL);
        }
        else {
          sub_timeval(timed_event->whence, __now, &__sleeptime);
          timed_event = NULL;
        }
      }
      pthread_mutex_unlock(&te_lock);
      if(timed_event == NULL) break;

      /* Make our call */
      newmask = timed_event->callback(timed_event, EVENTER_TIMER,
                                      timed_event->closure, &__now);
      if(newmask)
        eventer_add(timed_event);
      else
        eventer_free(timed_event);
    }

    if(compare_timeval(__max_sleeptime, __sleeptime) < 0) {
      /* we exceed our configured maximum, set it down */
      memcpy(&__sleeptime, &__max_sleeptime, sizeof(__sleeptime));
    }

    /* If we're the master, we need to lock the master_kqs and make mods */
    if(master_kqs->__ke_vec_used) {
      struct timespec __zerotime = { 0, 0 };
      pthread_mutex_lock(&kqs_lock);
      fd_cnt = kevent(kqueue_fd,
                      master_kqs->__ke_vec, master_kqs->__ke_vec_used,
                      NULL, 0,
                      &__zerotime);
      noit_log(noit_debug, &__now, "debug: kevent(%d, [], %d) => %d\n", kqueue_fd, master_kqs->__ke_vec_used, fd_cnt);
      if(fd_cnt < 0) {
        noit_log(noit_error, &__now, "kevent: %s\n", strerror(errno));
      }
      master_kqs->__ke_vec_used = 0;
      pthread_mutex_unlock(&kqs_lock);
    }

    /* Now we move on to our fd-based events */
    __kqueue_sleeptime.tv_sec = __sleeptime.tv_sec;
    __kqueue_sleeptime.tv_nsec = __sleeptime.tv_usec * 1000;
    fd_cnt = kevent(kqueue_fd, ke_vec, ke_vec_used,
                    ke_vec, ke_vec_a,
                    &__kqueue_sleeptime);
    noit_log(noit_debug, &__now, "debug: kevent(%d, [], %d) => %d\n", kqueue_fd, ke_vec_used, fd_cnt);
    ke_vec_used = 0;
    if(fd_cnt < 0) {
      noit_log(noit_error, &__now, "kevent: %s\n", strerror(errno));
    }
    else {
      int idx;
      for(idx = 0; idx < fd_cnt; idx++) {
        ev_lock_state_t lockstate;
        struct kevent *ke;
        eventer_t e;
        int fd, evmask, oldmask;

        ke = &ke_vec[idx];
        if(ke->flags & EV_ERROR) {
          if(ke->data != EBADF)
            noit_log(noit_error, &__now, "error: %s\n", strerror(ke->data));
          continue;
        }
        e = (eventer_t)ke->udata;
        fd = ke->ident;
        assert(e == master_fds[fd].e);
        lockstate = acquire_master_fd(fd);
        assert(lockstate == EV_OWNED);

        evmask = 0;
        if(ke->filter == EVFILT_READ) evmask = EVENTER_READ;
        if(ke->filter == EVFILT_WRITE) evmask = EVENTER_WRITE;
        gettimeofday(&__now, NULL);
        oldmask = e->mask;
        newmask = e->callback(e, evmask, e->closure, &__now);

        if(newmask) {
          /* toggle the read bits if needed */
          if(newmask & (EVENTER_READ | EVENTER_EXCEPTION)) {
            if(!(oldmask & (EVENTER_READ | EVENTER_EXCEPTION)))
              ke_change(fd, EVFILT_READ, EV_ADD | EV_ENABLE, e);
          }
          else if(oldmask & (EVENTER_READ | EVENTER_EXCEPTION))
            ke_change(fd, EVFILT_READ, EV_DELETE | EV_DISABLE, e);
  
          /* toggle the write bits if needed */
          if(newmask & EVENTER_WRITE) {
            if(!(oldmask & EVENTER_WRITE))
              ke_change(fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, e);
          }
          else if(oldmask & EVENTER_WRITE)
              ke_change(fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, e);
  
          /* Set our mask */
          e->mask = newmask;
        }
        else {
          eventer_free(e);
        }
        release_master_fd(fd, lockstate);
      }
    }
  }
}

struct _eventer_impl eventer_kqueue_impl = {
  "kqueue",
  eventer_kqueue_impl_init,
  eventer_kqueue_impl_propset,
  eventer_kqueue_impl_add,
  eventer_kqueue_impl_remove,
  eventer_kqueue_impl_update,
  eventer_kqueue_impl_remove_fd,
  eventer_kqueue_impl_loop
};
