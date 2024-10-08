/*
 * wepoll - epoll for Windows
 * https://github.com/piscisaureus/wepoll
 *
 * Copyright 2012-2020, Bert Belder <bertbelder@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef WEPOLL_H_
#define WEPOLL_H_

#include <io.h>
#include <stdint.h>
#include <winsock2.h>

enum EPOLL_EVENTS
{
  EPOLLIN      = (int) (1U <<  0),
  EPOLLPRI     = (int) (1U <<  1),
  EPOLLOUT     = (int) (1U <<  2),
  EPOLLERR     = (int) (1U <<  3),
  EPOLLHUP     = (int) (1U <<  4),
  EPOLLRDNORM  = (int) (1U <<  6),
  EPOLLRDBAND  = (int) (1U <<  7),
  EPOLLWRNORM  = (int) (1U <<  8),
  EPOLLWRBAND  = (int) (1U <<  9),
  EPOLLMSG     = (int) (1U << 10), /* Never reported. */
  EPOLLRDHUP   = (int) (1U << 13),
  EPOLLONESHOT = (int) (1U << 31)
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

typedef void     *HANDLE;
typedef uintptr_t SOCKET;

typedef union epoll_data {
        void    *ptr;
        int      fd;
        uint32_t u32;
        uint64_t u64;
        SOCKET   sock;       /* Windows specific */
        HANDLE   hnd;        /* Windows specific */
      } epoll_data_t;

struct epoll_event {
       uint32_t     events;  /* Epoll events and flags */
       epoll_data_t data;    /* User data variable */
     };

#ifdef __cplusplus
extern "C" {
#endif

int    _epoll_init (void);
HANDLE _epoll_create (int size, const char *file, unsigned line);
HANDLE _epoll_create1 (int flags, const char *file, unsigned line);

int _epoll_close (HANDLE ephnd, const char *file, unsigned line);

int _epoll_ctl (HANDLE              ephnd,
                int                 op,
                SOCKET              sock,
                struct epoll_event *event,
                const char         *file,
                unsigned            line);

int _epoll_wait (HANDLE              ephnd,
                 struct epoll_event *events,
                 int                 maxevents,
                 int                 timeout,
                 const char         *file,
                 unsigned            line);

int _eventfd (int init_val, int flags, const char *file, unsigned line);

#define epoll_create(size)                     (int) _epoll_create (size, __FILE__, __LINE__)
#define epoll_create1(flags)                   (int) _epoll_create1 (flags, __FILE__, __LINE__)
#define epoll_close(fd)                              _epoll_close ((HANDLE)(fd), __FILE__, __LINE__)
#define epoll_ctl(epfd, op, fd, event)               _epoll_ctl ((HANDLE)(epfd), op, fd, event, __FILE__, __LINE__)
#define epoll_wait(epfd, events, maxevent, timeout)  _epoll_wait ((HANDLE)(epfd), events, maxevent, timeout, __FILE__, __LINE__)
#define eventfd(init_val, flags)                     _eventfd (init_val, flags, __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif /* WEPOLL_H_ */
