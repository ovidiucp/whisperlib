// Copyright (c) 2009, Whispersoft s.r.l.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// * Neither the name of Whispersoft s.r.l. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Authors: Cosmin Tudorache, Catalin Popescu, Ovidiu Predescu
//
// Base for Selector - we have different implementations if we use
// poll - most portable, epoll - great for linux, kevents - great for bsd
//

#ifndef __NET_BASE_SELECTOR_BASE_H__
#define __NET_BASE_SELECTOR_BASE_H__

#include <whisperlib/base/core_config.h>

#include <vector>
#include <whisperlib/base/types.h>
#include <whisperlib/net/selector_event_data.h>

#include WHISPER_HASH_MAP_HEADER

//////////////////////////////////////////////////////////////////////
//
// EPOLL version
//
#if defined(HAVE_SYS_EPOLL_H)

#include <sys/epoll.h>

// We need this definde to a safe value anyway
#ifndef EPOLLRDHUP
// #define EPOLLRDHUP 0x2000
#define EPOLLRDHUP 0
#endif

#elif defined(HAVE_KQUEUE)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#elif defined(HAVE_POLL_H) || defined(HAVE_SYS_POLL_H)

#ifdef HAVE_POLL_H
#include "poll.h"
#else
#include <sys/poll.h>
#endif

// We need this definde to a safe value anyway
#ifndef POLLRDHUP
// #define POLLRDHUP 0x2000
#define POLLRDHUP 0
#endif

#else

#error "Need poll or epoll available"

#endif

namespace io {
  class File;
}

//////////////////////////////////////////////////////////////////////

// COMMON part

namespace net {

class Selectable;

class SelectorBase {
 public:

  SelectorBase(int pipe_fd, int max_events_per_step);
  ~SelectorBase();

  //  add a file descriptor in the epoll, and link to the given data pointer.
  bool Add(int fd, void* user_data, int desires);
  //  updates the desires for the file descriptor in the epoll.
  bool Update(int fd, void* user_data, int desires);
  //  remove a file descriptor from the epoll
  bool Delete(int fd);

  // Run a selector loop step. It fills in events two things:
  //  -- the user data associted with the fd that was triggered
  //  -- the event that happended (an or of Selector desires)
  bool LoopStep(int32 timeout_in_ms,
                vector<SelectorEventData>*  events);

private:
  const int max_events_per_step_;

#if defined(HAVE_SYS_EPOLL_H)
  //////////////////////////////////////////////////////////////////////
  //
  // EPOLL version
  //

  // Converts a Selector desire in some epoll flags.
  int DesiresToEpollEvents(int32 desires);

  // epoll file descriptor
  const int epfd_;

  // here we get events that we poll
  struct epoll_event* const events_;

#elif defined(HAVE_KQUEUE)
  //////////////////////////////////////////////////////////////////////
  //
  // KQUEUE version
  //

  // Maximum number of events we're willing to process in a single go.
  static const int kMaxProcessed = 1024;

  // The event queue
  int kq_;

# if defined(HAVE_KEVENT64)
  vector<kevent64_s> events_;
  struct kevent64_s fdEvents_[kMaxProcessed];
# else
  vector<kevent> events_;
  struct kevent fdEvents_[kMaxProcessed];
# endif

  //  Updates the (fd, kevent_type) in kq_. This is done by adding the
  //  (fd, kevent_type) to the events_ array, which is passed to
  //  kevent() system call in LoopStep().
  //
  // `kevent_type` is determined by `desires` in the following way:
  //
  // desires & kWantRead will enter in kqueue (fd, EVFILT_READ)
  // desires & kWantWrite will enter in kqueue (fd, EVFILT_WRITE)
  // desires & kWantError will enter in kqueue two entries:
  //    (fd, EVFILT_READ) and (fd, EVFILT_WRITE)
  //
  // Action is EV_ADD or EV_DELETE.
  bool _Add(int fd, void* user_data, int action, int32 desires);

#else
  //////////////////////////////////////////////////////////////////////
  //
  // POLL version
  //

  // Compacts the fds table at the end of a loop step
  void Compact();

  static const int kMaxFds = 4096;

  // how many in fds are used
  size_t fds_size_;

  // epoll file descriptor
  struct pollfd fds_[kMaxFds];

  // maps from fd to index in fds_ and user data
  typedef hash_map< int, pair<size_t, void*> > DataMap;
  DataMap fd_data_;

  // Converts a Selector desire in some poll flags.
  int DesiresToPollEvents(int32 desires);

  // indices that we need to compact at the end of the step
  vector<size_t> indices_to_compact_;
#endif

  //////////////////////////////////////////////////////////////////////

  DISALLOW_EVIL_CONSTRUCTORS(SelectorBase);
};

}

#endif  // __NET_BASE_SELECTOR_BASE_H__
