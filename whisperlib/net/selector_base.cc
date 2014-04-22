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

#include <whisperlib/base/core_errno.h>
#include <whisperlib/net/selector_base.h>
#include <whisperlib/net/selector.h>
#include <whisperlib/io/file/file.h>
#include <unistd.h>
#include <algorithm>
#include <string>

namespace net {

//////////////////////////////////////////////////////////////////////
//
// EPOLL version
//
#if defined(HAVE_SYS_EPOLL_H)

SelectorBase::SelectorBase(int pipe_fd, int max_events_per_step)
    : max_events_per_step_(max_events_per_step),
      epfd_(epoll_create(10)),
      events_(new epoll_event[max_events_per_step]) {
  CHECK(epfd_ >= 0) << "epoll_create() failed: "
                    << GetLastSystemErrorDescription();
  CHECK(Add(pipe_fd, NULL, Selector::kWantRead | Selector::kWantError));
}

SelectorBase::~SelectorBase() {
  // cleanup epoll
  ::close(epfd_);
  delete[] events_;
}

bool SelectorBase::Add(int fd, void* user_data, int32 desires) {
  // Insert in epoll
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  epoll_event event = { static_cast<unsigned int>(DesiresToEpollEvents(desires)), };
  event.data.ptr = user_data;

  DLOG_INFO << "  Adding to epoll: " << fd;
  if ( epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) < 0 ) {
    LOG_ERROR  << "System error on epoll_ctl: "
               << GetLastSystemErrorDescription()
               << " for events: " << hex << event.events << dec;
    return false;
  }
  return true;
}

bool SelectorBase::Update(int fd, void* user_data, int32 desires) {
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  epoll_event event = { static_cast<unsigned int>(DesiresToEpollEvents(desires)), };
  event.data.ptr = user_data;

  if ( epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) ) {
    LOG_ERROR << "Error in epoll_ctl: "  << GetLastSystemErrorDescription();
    return false;
  }
  return true;
}

bool SelectorBase::Delete(int fd) {
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  epoll_event event = { 0, };
  DLOG_INFO << "  Removing from epoll: " << fd;
  if ( epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &event) < 0 ) {
    LOG_ERROR  << "System error on epoll_ctl: "
               << GetLastSystemErrorDescription()
               << " for fd: " << fd;
    return false;
  }
  return true;
}


int SelectorBase::DesiresToEpollEvents (int32 desires) {
  int events = 0;
  if ( desires & Selector::kWantRead )
    events |= EPOLLIN | EPOLLRDHUP;
  if ( desires & Selector::kWantWrite )
    events |= EPOLLOUT;
  if ( desires & Selector::kWantError )
    events |= EPOLLERR | EPOLLHUP;
  return events;
}

bool SelectorBase::LoopStep(int32 timeout_in_ms,
                            vector<SelectorEventData>*  events) {
  const int num_events = epoll_wait(epfd_, events_, max_events_per_step_,
                                    timeout_in_ms);
  if ( num_events == -1 && errno != EINTR ) {
    LOG_ERROR << "epoll_wait() error: " << GetLastSystemErrorDescription();
    return false;
  }
  for ( int i = 0; i < num_events; ++i ) {
    struct epoll_event* event = (events_ + i);
    int32 desire = 0;
    if ( event->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP) ) {
      desire |= Selector::kWantError;
    }
    if ( event->events & (EPOLLIN | EPOLLPRI) ) {
      desire |= Selector::kWantRead;
    }
    if ( event->events & EPOLLOUT ) {
      desire |= Selector::kWantWrite;
    }
    events->push_back(SelectorEventData(event->data.ptr,
                                        desire,
                                        event->events));
  }
  return true;
}

//////////////////////////////////////////////////////////////////////


#elif defined(HAVE_KQUEUE)

//////////////////////////////////////////////////////////////////////
//
// KQUEUE version
//
SelectorBase::SelectorBase(int pipe_fd, int max_events_per_step)
    : max_events_per_step_(max_events_per_step) {
  CHECK(Add(pipe_fd, NULL, Selector::kWantRead | Selector::kWantError));

  kq_ = kqueue();
  if (kq_ == -1) {
    LOG_ERROR << "Cannot create kqueue: " << strerror(errno);
  }
}

SelectorBase::~SelectorBase() {
}

bool SelectorBase::_Add(int fd, void* user_data, int action, int32 desires) {
  DLOG_INFO << "Add file descriptor fd " << fd << " for action "
            << std::hex << action;
  if ( fd == INVALID_FD_VALUE ) {
    return false;
  }

#if defined(HAVE_KEVENT64)
  kevent64_s kev;
  if (desires & Selector::kWantRead) {
    EV_SET64(&kev, fd, EVFILT_READ, action,
             0 /* fflags */, 0 /* filter data */, (uint64_t)user_data, 0, 0);
    events_.push_back(kev);
  }
  if (desires & Selector::kWantWrite) {
    EV_SET64(&kev, fd, EVFILT_WRITE, action,
             0 /* fflags */, 0 /* filter data */, (uint64_t)user_data, 0, 0);
    events_.push_back(kev);
  }
  if (desires & Selector::kWantMonitor) {
    LOG_INFO << "Adding change notification to fd " << fd;
    EV_SET64(&kev, fd, EVFILT_VNODE, action | EV_CLEAR,
             (NOTE_DELETE |  NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB |
              NOTE_LINK | NOTE_RENAME | NOTE_REVOKE) /* fflags */,
             0 /* filter data */, (uint64_t)user_data, 0, 0);
    events_.push_back(kev);
  }
#else /* Regular kevent */
  kevent kev;
  if (desires & Selector::kWantRead) {
    EV_SET(&kev, fd, EVFILT_READ, action,
           0 /* fflags */, 0 /* filter data */, user_data);
    events_.push_back(kev);
  }
  if (desires & Selector::kWantWrite) {
    EV_SET(&kev, fd, EVFILT_WRITE, action,
           0 /* fflags */, 0 /* filter data */, user_data);
    events_.push_back(kev);
  }
  if (desires & Selector::kWantMonitor) {
    EV_SET(&kev, fd, EVFILT_VNODE, action | EV_CLEAR,
           (NOTE_DELETE |  NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB |
            NOTE_LINK | NOTE_RENAME | NOTE_REVOKE) /* fflags */,
           0 /* filter data */, user_data);
    events_.push_back(kev);
  }
#endif

  return true;
}

bool SelectorBase::Add(int fd, void* user_data, int32 desires) {
  return _Add(fd, user_data, EV_ADD, desires);
}

bool SelectorBase::Update(int fd, void* user_data, int32 desires) {
  return _Add(fd, user_data, EV_ADD, desires);
}

bool SelectorBase::Delete(int fd) {
  return _Add(fd, NULL, EV_DELETE,
              (Selector::kWantRead | Selector::kWantWrite));
}

bool SelectorBase::LoopStep(int32 timeout_in_ms,
                            vector<SelectorEventData>*  events) {
  // Wait for event notifications from the kernel
  struct timespec timeout;
  timeout.tv_sec = timeout_in_ms / 1000;
  timeout.tv_nsec = (timeout_in_ms % 1000) * 1e6;

  DLOG_INFO << "Going to add " << events_.size() << " new events to kqueue, "
            << "and listen for events";

#if defined(HAVE_KEVENT64)
  int num_events = kevent64(kq_, events_.data(), events_.size(),
                            fdEvents_, kMaxProcessed, 0, &timeout);
# else
  int num_events = kevent(kq_, events_.data(), events_.size(),
                          fdEvents_, kMaxProcessed, &timeout);
#endif
  DLOG_INFO << "Got events on " << num_events << " fds";

  // Clear the events we asked kqueue to listen for.
  events_.clear();

  // Iterate over the event notifications.
  for (int i=0; i < num_events; i++) {
# if defined(HAVE_KEVENT64)
    const struct kevent64_s& event = fdEvents_[i];
# else
    const struct kevent& event = fdEvents_[i];
# endif

    int32 desire = 0;
    DLOG_INFO << "Event on fd " << event.ident
              << ", filter: " << event.filter
              << ", flags: 0x" << std::hex << event.flags
              << ", fflags: 0x" << std::hex << event.fflags
              << ", error data: 0x:" << std::hex << event.data
              << " (" << strerror(event.data) << ")";
    switch (event.filter) {
      case EVFILT_READ:
        desire = Selector::kWantRead;
        break;
      case EVFILT_WRITE:
        desire = Selector::kWantWrite;
        break;
      case EVFILT_VNODE:
        desire = Selector::kWantMonitor;
        break;
      default:
        LOG_ERROR << "Unknown filter type for kevent: " << event.filter;
        break;
    }
    if (desire) {
      events->push_back(SelectorEventData((void*)event.udata,
                                          desire,
                                          event.fflags,
                                          event.data));
    }

    desire = 0;
    switch (event.flags) {
      case EV_EOF:
      case EV_ERROR:
        desire = Selector::kWantError;
        LOG_ERROR << "Got kqueue error 0x" << std::hex << event.flags
                  << " on fd " << event.ident;
        break;
      default:
        // We only care about the returned flags, so ignore the flags
        // that define the actions.
        break;
    }

    if (desire) {
      DLOG_INFO << "Pushing SelectorEventData for error desire " << desire
               << " event data 0x" << std::hex << event.data;
      events->push_back(SelectorEventData((void*)event.udata,
                                          desire,
                                          event.flags,
                                          event.data));
    }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////


#else

//////////////////////////////////////////////////////////////////////
//
// POLL version
//

SelectorBase::SelectorBase(int pipe_fd, int max_events_per_step)
    : max_events_per_step_(max_events_per_step),
      fds_size_(0) {
  CHECK(Add(pipe_fd, NULL, Selector::kWantRead | Selector::kWantError));
}

SelectorBase::~SelectorBase() {
}

bool SelectorBase::Add(int fd, void* user_data, int32 desires) {
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  if (fds_size_ >= kMaxFds) {
    LOG_ERROR << " Too many fds too listen to in poll";
    return false;
  }
  fds_[fds_size_].fd = fd;
  fds_[fds_size_].events = DesiresToPollEvents(desires);
  fds_[fds_size_].revents = 0;
  fd_data_.insert(make_pair(fd, make_pair(fds_size_, user_data)));
  ++fds_size_;
  return true;
}

bool SelectorBase::Update(int fd, void* user_data, int32 desires) {
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  const DataMap::iterator it = fd_data_.find(fd);
  if (it == fd_data_.end()) {
    LOG_ERROR << "Cannot find poll struct for: " << fd;
    return false;
  }
  const size_t index = it->second.first;
  fds_[index].events = DesiresToPollEvents(desires);
  return true;
}

bool SelectorBase::Delete(int fd) {
  if ( fd == INVALID_FD_VALUE ) {
    return true;
  }
  const DataMap::iterator it = fd_data_.find(fd);
  if (it == fd_data_.end()) {
    LOG_ERROR << "Cannot find poll struct for: " << fd;
    return false;
  }
  const size_t index = it->second.first;
  // We don't compact now, as we may loose processing for these events
  // if we do this in the middle of a step.
  indices_to_compact_.push_back(index);

  // However we delete the fd
  fds_[index].fd = -1;
  fd_data_.erase(it);
  return true;
}

void SelectorBase::Compact() {
  if (indices_to_compact_.empty()) {
        return;
  }
  ::sort(indices_to_compact_.begin(), indices_to_compact_.end());
  for ( int i = indices_to_compact_.size() - 1; i >= 0; --i ) {
    const size_t index = indices_to_compact_[i];
    --fds_size_;
    if (fds_size_ > 0 && index != fds_size_) {
      // Move the last poll structure in the one freed by the deleted
      const DataMap::iterator it = fd_data_.find(fds_[fds_size_].fd);
      CHECK(it != fd_data_.end());
      it->second.first = index;
      fds_[index].fd = fds_[fds_size_].fd;
      fds_[index].events = fds_[fds_size_].events;
      fds_[index].revents = 0;
    }
  }
  indices_to_compact_.clear();
}


int SelectorBase::DesiresToPollEvents(int32 desires) {
  int events = 0;
  if ( desires & Selector::kWantRead )
    events |= POLLIN | POLLRDHUP;
  if ( desires & Selector::kWantWrite )
    events |= POLLOUT;
  if ( desires & Selector::kWantError )
    events |= POLLERR | POLLHUP;
  return events;
}

bool SelectorBase::LoopStep(int32 timeout_in_ms,
                            vector<SelectorEventData>*  events) {
  Compact();
  int num_events = poll(fds_, fds_size_, timeout_in_ms);
  if ( num_events == -1 && errno != EINTR ) {
    LOG_ERROR << "epoll_wait() error: " << GetLastSystemErrorDescription();
    return false;
  }
  for ( int i = 0; i < fds_size_ && num_events > 0 ; ++i ) {
    const struct pollfd& event = fds_[i];
    if (event.revents == 0) {
      continue;
    }
    int32 desire = 0;
    if ( event.revents & (POLLERR | POLLHUP | POLLRDHUP | POLLNVAL) ) {
      desire |= Selector::kWantError;
    }
    if ( event.revents & (POLLIN | POLLPRI) ) {
      desire |= Selector::kWantRead;
    }
    if ( event.events & POLLOUT ) {
      desire |= Selector::kWantWrite;
    }
    const DataMap::iterator it = fd_data_.find(event.fd);
    CHECK(it != fd_data_.end());
    events->push_back(SelectorEventData(it->second.second,
                                        desire,
                                        event.revents));
    --num_events;
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

#endif

}
