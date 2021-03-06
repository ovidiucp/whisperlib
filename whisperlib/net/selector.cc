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
// Authors: Cosmin Tudorache & Catalin Popescu

#include "whisperlib/net/selector.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#ifdef HAVE_EVENTFD_H
#include <sys/eventfd.h>
#endif

#include <whisperlib/base/log.h>
#include <whisperlib/base/core_errno.h>
#include <whisperlib/base/timer.h>
#include <whisperlib/base/gflags.h>
#include <whisperlib/net/selectable.h>
#include <whisperlib/net/selectable_filereader.h>
#include <whisperlib/io/file/file.h>

//////////////////////////////////////////////////////////////////////

DEFINE_bool(selector_high_alarm_precission,
            false,
            "Loose some CPU time and gain that extra milisecond precission "
            "for selector alarms..");
DEFINE_int32(selector_num_closures_per_event,
             64,
             "We don't run more than these many closures per event");
DEFINE_int32(selector_events_to_read_per_poll,
             64,
             "We process at most these many events per poll step");
DEFINE_int32(debug_check_long_callbacks_ms,
             500,
             "If greater than zero, we check (in debug mode only !) "
             "that processing functions, callbacks and alarm functions take "
             "less then this amount of time, in miliseconds");

//////////////////////////////////////////////////////////////////////

namespace {
void StopSignal() {
}
Closure* glb_stop_signal = NewPermanentCallback(&StopSignal);
static const int kCallbackQueueSize = 10000;
}

namespace net {

Selector::Selector()
  : tid_(0),
    should_end_(false),
#ifdef __USE_LEAN_SELECTOR__
    to_run_(kCallbackQueueSize),
#else
    base_(NULL),
#endif //   __USE_LEAN_SELECTOR__
    now_(timer::TicksMsec()),
    call_on_close_(NULL) {
#ifndef  __USE_LEAN_SELECTOR__
#ifdef HAVE_EVENTFD_H
  event_fd_ = eventfd(0, 0);
  CHECK(event_fd_ >= 0)
      << " eventfd fail: " << GetLastSystemErrorDescription();
  const int flags = fcntl(event_fd_, F_GETFL, 0);
  CHECK(fcntl(event_fd_, F_SETFL, flags | O_NONBLOCK) >= 0)
      << " fcntl fail: " << GetLastSystemErrorDescription();
#else
  CHECK(!pipe(signal_pipe_)) <<  "pipe() failed:  "
                             << GetLastSystemErrorDescription();
  for ( int i = 0; i < NUMBEROF(signal_pipe_); ++i ) {
    const int flags = fcntl(signal_pipe_[i], F_GETFL, 0);
    CHECK(flags >= 0) << " fcntl fail: " << GetLastSystemErrorDescription();
    CHECK(fcntl(signal_pipe_[i], F_SETFL, flags | O_NONBLOCK) >= 0)
         << " fcntl fail: " << GetLastSystemErrorDescription();
  }
  event_fd_ = signal_pipe_[0];
#endif
  base_ = new SelectorBase(event_fd_,
                           FLAGS_selector_events_to_read_per_poll);
#endif   //  __USE_LEAN_SELECTOR__

#if defined(HAVE_SYS_INOTIFY_H)
  _InitializeInotify();
#endif
}

Selector::~Selector() {
  CHECK(tid_ == 0);
  CHECK(registered_.empty());

#if defined(HAVE_SYS_INOTIFY_H)
  _DestroyInotify();
#endif

#if defined(HAVE_KQUEUE)
  _DestroyKqueueNotifiers();
#endif

#ifndef __USE_LEAN_SELECTOR__
#ifdef HAVE_EVENTFD_H
  close(event_fd_);
#else
  close(signal_pipe_[0]);
  close(signal_pipe_[1]);
#endif
#endif   // __USE_LEAN_SELECTOR__
}

void Selector::Loop() {
  CHECK(tid_ ==  0) << "Loop already started -- bad !";
  should_end_ = false;
  tid_ = pthread_self();
  LOG_INFO << "Starting selector loop";

  vector<SelectorEventData> events;
  while ( !should_end_ ) {
    int32 to_sleep_ms = kStandardWakeUpTimeMs;
    if ( FLAGS_selector_high_alarm_precission ) {
      now_ = timer::TicksMsec();
    }
    if ( !alarms_.empty() ) {
      if ( alarms_.begin()->first < now_ + to_sleep_ms ) {
        to_sleep_ms = alarms_.begin()->first - now_;
      }
    }

    int run_count = 0;
#ifdef __USE_LEAN_SELECTOR__
    Closure* next_to_run = to_run_.Get(to_sleep_ms);
    if (glb_stop_signal == next_to_run) {
        should_end_ = true;
    } else if (next_to_run) {
        now_ = timer::TicksMsec();
        next_to_run->Run();
        ++run_count;
    }
#else          // __USE_LEAN_SELECTOR__
    if ( !to_run_.empty() ) {
      to_sleep_ms = 0;
    }
    events.clear();
    if (!base_->LoopStep(to_sleep_ms, &events)) {
      LOG_ERROR << "ERROR in select loop step. Exiting Loop.";
      break;
    }
    now_ = timer::TicksMsec();
#ifdef _DEBUG
    const int64 processing_begin =
        FLAGS_debug_check_long_callbacks_ms > 0 ? timer::TicksMsec() : 0;
#endif
    for ( int i = 0; i < events.size(); ++i ) {
      const SelectorEventData& event = events[i];
      Selectable* const s = reinterpret_cast<Selectable *>(event.data_);
      if ( s == NULL ) {
        // was probably a wake signal..
        continue;
      }
      if ( s->selector() == NULL ) {
        // already unregistered
        continue;
      }
      const int32 desire = event.desires_;
      // During HandleXEvent the obj may be closed loosing so track of
      // it's fd value.
      bool keep_processing = true;
      if ( desire & kWantError ) {
        keep_processing = s->HandleErrorEvent(event) &&
                          s->GetFd() != INVALID_FD_VALUE;
      }
      if ( keep_processing && (desire & kWantRead) ) {
        keep_processing = s->HandleReadEvent(event) &&
                          s->GetFd() != INVALID_FD_VALUE;
      }
      if ( keep_processing && (desire & kWantWrite) ) {
        s->HandleWriteEvent(event);
      }
#if defined(HAVE_KQUEUE)
      if ( keep_processing && (desire & kWantMonitor) ) {
        keep_processing = (s->HandleReadEvent(event) &&
                           s->GetFd() != INVALID_FD_VALUE);
      }
#endif
    }  // else, was probably a timeout
#ifdef _DEBUG
    if ( FLAGS_debug_check_long_callbacks_ms > 0 ) {
      const int64 processing_end = timer::TicksMsec();
      if ( processing_end - processing_begin >
           FLAGS_debug_check_long_callbacks_ms ) {
        LOG_ERROR << this << " ====>> Unexpectedly long event processing: "
                  << " time spent:  "
                  << processing_end - processing_begin
                  << " num events: " << events.size();
      }
    }
#endif

    // Runs some closures..
    if ( FLAGS_selector_high_alarm_precission ) {
      now_ = timer::TicksMsec();
    }
    while ( run_count < FLAGS_selector_num_closures_per_event ) {
      int n = RunClosures(FLAGS_selector_num_closures_per_event);
      if ( n == 0 ) {
        break;
      }
      run_count += n;
    }
#endif     //  __USE_LEAN_SELECTOR__

    // Run the alarms
    if ( FLAGS_selector_high_alarm_precission ) {
      now_ = timer::TicksMsec();
    }
    while ( !alarms_.empty() ) {
      if ( alarms_.begin()->first > now_ ) {
        // this alarm and all that follow are in the future
        break;
      }
      Closure* closure = alarms_.begin()->second;
      const bool erased = reverse_alarms_.erase(closure);
      CHECK(erased);
      run_count++;
      alarms_.erase(alarms_.begin());
#ifdef _DEBUG
      const int64 processing_begin =
          FLAGS_debug_check_long_callbacks_ms > 0 ? timer::TicksMsec() : 0;
#endif
      closure->Run();
#ifdef _DEBUG
      if ( FLAGS_debug_check_long_callbacks_ms > 0 ) {
        const int64 processing_end = timer::TicksMsec();
        if ( processing_end - processing_begin >
             FLAGS_debug_check_long_callbacks_ms ) {
          LOG_ERROR << this << " ====>> Unexpectedly long alarm processing: "
                    << " callback: " << closure
                    << " time spent:  "
                    << processing_end - processing_begin;
        }
      }
#endif
    }
    if ( run_count > 2 * FLAGS_selector_num_closures_per_event ) {
      LOG_ERROR << this << " We run to many closures per event: " << run_count;
    }
  }

  LOG_INFO << "Closing all the active connections in the selector...";
  CleanAndCloseAll();
  CHECK(registered_.empty());

  // Run all the remaining closures...
  now_ = timer::TicksMsec();
  int run_count = 0;
  while ( true ) {
    int n = RunClosures(FLAGS_selector_num_closures_per_event);
    if ( n == 0 ) {
      break;
    }
    run_count += n;
    LOG_INFO << "Running closures on shutdown, count: " << run_count;
  }
#ifndef __USE_LEAN_SELECTOR__
  CHECK(to_run_.empty());
#endif

  // Run remaining alarms
  while ( !alarms_.empty() ) {
    now_ = timer::TicksMsec();
    if ( alarms_.begin()->first > now_ ) {
      // this alarm and all that follow are in the future
      break;
    }
    Closure* closure = alarms_.begin()->second;
    const bool erased = reverse_alarms_.erase(closure);
    CHECK(erased);
    alarms_.erase(alarms_.begin());
  }
  if ( !alarms_.empty() ) {
    LOG_ERROR << "Now: " << now_ << " ms";
  }
  for ( AlarmSet::const_iterator it = alarms_.begin(); it != alarms_.end();
        ++it ) {
    LOG_ERROR << "Leaking alarm, run at: " << it->first
              << " ms, due in: " << (it->first - now_) << " ms";
  }

#ifndef __USE_LEAN_SELECTOR__
  delete base_;
  base_ = NULL;
#endif

  if ( call_on_close_ != NULL ) {
    call_on_close_->Run();
    call_on_close_ = NULL;
  }
  tid_ = 0;
}

void Selector::MakeLoopExit() {
  CHECK(IsInSelectThread());
#ifdef __USE_LEAN_SELECTOR__
  to_run_.Put(glb_stop_signal);
#else
  should_end_ = true;
#endif
}

void Selector::RunInSelectLoop(Closure* callback) {
  DCHECK(tid_ != 0 || !should_end_) << "Selector already stopped";
  CHECK_NOT_NULL(callback);
  #ifdef _DEBUG
  callback->set_selector_registered(true);
  #endif

#ifdef __USE_LEAN_SELECTOR__
  to_run_.Put(callback);
#else
  mutex_.Lock();
  to_run_.push_back(callback);
  mutex_.Unlock();
  if ( !IsInSelectThread() ) {
    SendWakeSignal();
  }
#endif
}
void Selector::RegisterAlarm(Closure* callback, int64 timeout_in_ms) {
  DCHECK(tid_ != 0 || !should_end_) << "Selector already stopped";
  CHECK(IsInSelectThread() || (tid_ == 0 && !should_end_));
  CHECK_NOT_NULL(callback);
  const int64 wake_up_time = now_ + timeout_in_ms;
  CHECK(timeout_in_ms < 0 || timeout_in_ms <= wake_up_time)
    << "Overflow, timeout_in_ms: " << timeout_in_ms << " is too big";

  pair<ReverseAlarmsMap::iterator, bool> result =
      reverse_alarms_.insert(make_pair(callback, wake_up_time));
  if ( result.second ) {
    // New alarm inserted; Now add to alarms_ too.
    alarms_.insert(make_pair(wake_up_time, callback));
  } else {
    // Old alarm needs replacement.
    alarms_.erase(make_pair(result.first->second, callback));
    alarms_.insert(make_pair(wake_up_time, callback));
    result.first->second = wake_up_time;
  }
  // We do not need to wake .. we are in the select loop :)
}
void Selector::UnregisterAlarm(Closure* callback) {
  CHECK(IsInSelectThread());
  ReverseAlarmsMap::iterator it = reverse_alarms_.find(callback);
  if ( it != reverse_alarms_.end() ) {
    int count = alarms_.erase(make_pair(it->second, callback));
    CHECK_EQ(count,1);
    reverse_alarms_.erase(it);
  }
}

//////////////////////////////////////////////////////////////////////

void Selector::CleanAndCloseAll() {
  // It is some discussion, whether to do some CHECK if connections are
  // left at this point or to close them or to just skip it. The ideea is
  // that we preffer forcing a close on them for the server case and also
  // client connections when we end a program.
  if ( !registered_.empty() ) {
    DLOG_INFO << "Select loop ended with " << registered_.size()
              << " registered connections.";
    // We just close the fd and care about nothing ..
    while ( !registered_.empty() ) {
      (*registered_.begin())->Close();
    }
  }
}

int Selector::RunClosures(int num_closures) {
#ifdef __USE_LEAN_SELECTOR__
    int run_count = 0;
    while (run_count < num_closures) {
        Closure* next_to_run = to_run_.Get(0);
        if (glb_stop_signal != next_to_run && next_to_run) {
#ifdef _DEBUG
            closure->set_selector_registered(false);
#endif
            ++run_count;
        } else {
            return run_count;
        }
    }
    return run_count;


#else  // __USE_LEAN_SELECTOR__
#ifdef HAVE_EVENTFD_H
  char buffer[1024];
#else
  char buffer[32];
#endif

  int cb = 0;
  while ( (cb = ::read(event_fd_, buffer, sizeof(buffer))) > 0 ) {
    VLOG(10) << " Cleaned some " << cb << " bytes from signal pipe.";
  }
  int run_count = 0;
  while ( run_count < FLAGS_selector_num_closures_per_event ) {
    mutex_.Lock();
    if ( to_run_.empty() ) {
      mutex_.Unlock();
      break;
    }
    Closure* closure = to_run_.front();
    to_run_.pop_front();
    mutex_.Unlock();

#ifdef _DEBUG
    const int64 processing_begin = FLAGS_debug_check_long_callbacks_ms > 0 ?
        timer::TicksMsec() : 0;
    closure->set_selector_registered(false);
#endif
    closure->Run();
    run_count++;
#ifdef _DEBUG
    if ( FLAGS_debug_check_long_callbacks_ms > 0 ) {
      const int64 processing_end = timer::TicksMsec();
      if ( processing_end - processing_begin >
           FLAGS_debug_check_long_callbacks_ms ) {
        LOG_ERROR << this << " ====>> Unexpectedly long callback processing: "
                  << " callback: " << closure
                  << " time spent:  "
                  << processing_end - processing_begin;
      }
    }
#endif
  }
#endif  // __USE_LEAN_SELECTOR__
  return run_count;
}

void Selector::SendWakeSignal() {
#ifndef  __USE_LEAN_SELECTOR__
#ifdef HAVE_EVENTFD_H
  uint64 value = 1ULL;
  if ( sizeof(value) != ::write(event_fd_, &value, sizeof(value)) ) {
    LOG_ERROR << "Error writing a wake-up byte in selector event_fd_";
  }
#else
  int8 byte = 0;
  if ( sizeof(byte) != ::write(signal_pipe_[1], &byte, sizeof(byte)) ) {
    LOG_ERROR << "Error writing a wake-up byte in selector signal pipe";
  }
#endif
#endif  // __USE_LEAN_SELECTOR__
}

//////////////////////////////////////////////////////////////////////

bool Selector::Register(Selectable* s) {
#ifdef __USE_LEAN_SELECTOR__
  CHECK(false) << " Cannot use selectables in this mode";
  return false;
#else
  DCHECK(IsInSelectThread() || tid_ == 0);
  CHECK(s->selector() == this);
  const int fd = s->GetFd();

  SelectableSet::const_iterator it = registered_.find(s);
  if ( it != registered_.end() ) {
    // selectable obj already registered
    CHECK_EQ((*it)->GetFd(), fd);
    return true;
  }
  // Insert in the local set of registered objs
  registered_.insert(s);

  return base_->Add(fd, s, s->desire_);
#endif   //  __USE_LEAN_SELECTOR__
}

void Selector::Unregister(Selectable* s) {
#ifdef __USE_LEAN_SELECTOR__
  CHECK(false) << " Cannot use selectables in this mode";
#else
  DCHECK(IsInSelectThread() || tid_ == 0);
  CHECK(s->selector() == this);
  const int fd = s->GetFd();

  const SelectableSet::iterator it = registered_.find(s);
  DCHECK(it != registered_.end()) << " Cannot unregister : " << fd;

  base_->Delete(fd);
  registered_.erase(it);
  s->set_selector(NULL);
#endif   //  __USE_LEAN_SELECTOR__
}

//////////////////////////////////////////////////////////////////////

void Selector::UpdateDesire(Selectable* s, bool enable, int32 desire) {
  // DCHECK(registered_.find(s) != registered_.end()) << fd;
  if ( ((s->desire_ & desire) && enable) ||
       ((~s->desire_ & desire) && !enable) ) {
    return;  // already set ..
  }
  if ( enable ) {
    s->desire_ |= desire;
  } else {
    s->desire_ &= ~desire;
  }
#ifndef __USE_LEAN_SELECTOR__
  base_->Update(s->GetFd(), s, s->desire_);
#endif
}


//////////////////////////////////////////////////////////////////////
//
// File change notification support
//


//////////////////////////////////////////////////////////////////////
//
// Linux inotify
//

#if defined(HAVE_SYS_INOTIFY_H)

void Selector::_InitializeInotify() {
  ifd_ = inotify_init();
  ifd_reader_ = new SelectableFilereader(this);
  ifd_reader_->InitializeFd(
      ifd_,
      NewPermanentCallback(this, &Selector::_HandleChangeNotification),
      NULL);
}

void Selector::_DestroyInotify() {
  delete ifd_reader_;
  close(ifd_);
}

void Selector::_HandleChangeNotification(io::MemoryStream* in) {
  int pos = 0;
  int size;
  struct inotify_event ev;
  while ((size = in->Read(&ev, sizeof(struct inotify_event))) > 0) {
    if (size != sizeof(struct inotify_event)) {
      LOG_ERROR << "Invalid amount of data read from inotify file descriptor: "
                << size << " bytes, ignoring";
      break;
    }
    if (ev.len > PATH_MAX) {
      LOG_ERROR << "Returned filename is longer than allowed by the system!";
      break;
    }
    if (ev.len == 0) {
      LOG_INFO << "Invalid empty file name read for inotify event, ignoring";
    } else {
      // The filename is specified, read it from the buffer, so we can properly
      // process the rest of the notification events.
      char filename[ev.len + 1];
      size = in->Read(filename, ev.len);
    }

    // At this point we read a complete notification event. Look up
    // the file object given the file descriptor.
    const InotifyDescriptorToFileMap::iterator it = ifd_map_.find(ev.wd);
    if (it == ifd_map_.end()) {
      LOG_ERROR << "Could not find a File object registered for file descriptor"
                << ev.wd << ", ignoring.";
      continue;
    }
    io::File* file = it->second;
    // Find the handler associated with this file object
    const InotifyFileToHandlerMap::iterator it2 = ihandler_map_.find(file);
    if (it2 == ihandler_map_.end()) {
      LOG_ERROR << "Could not find callback to invoke for change event on file "
                << file->filename() << ", ignoring.";
      continue;
    }
    FileChangedHandler* handler = it2->second;
    CHECK(handler);
    handler->Run(this, file, _WatchMaskToEvent(ev.mask));
  }
}

int Selector::_WatchMaskToEvent(int evmask) {
  int result = 0;
  if (evmask & IN_DELETE_SELF) {
    evmask ^= IN_DELETE_SELF;
    result |= Selector::kFileDeleted;
  }
  if (evmask & IN_MODIFY) {
    evmask ^= IN_MODIFY;
    result |= Selector::kFileModified;
  }
  if (evmask & IN_ATTRIB) {
    evmask ^= IN_ATTRIB;
    result |= Selector::kFileAttributes;
  }
  if (evmask & IN_MOVE_SELF) {
    evmask ^= IN_MOVE_SELF;
    result |= Selector::kFileRenamed;
  }
  if (evmask) {
    result |= Selector::kFileOther;
  }
  return result;
}

bool Selector::RegisterFileForChangeNotifications(io::File* file,
                                                  FileChangedHandler* handler)
{
  CHECK(file);
  CHECK(handler && handler->is_permanent());

  const InotifyFileToDescriptorMap::iterator it = ifile_map_.find(file);
  if (it != ifile_map_.end()) {
    LOG_INFO << "File has already been registered for change notifications, "
             << "ignoring: " << file->filename();
    return false;
  }

  int wfd = inotify_add_watch(ifd_, file->filename().c_str(), IN_ALL_EVENTS);
  ifile_map_.insert(make_pair(file, wfd));
  ifd_map_.insert(make_pair(wfd, file));
  ihandler_map_.insert(make_pair(file, handler));
  return true;
}

bool Selector::UnregisterFileForChangeNotifications(io::File* file)
{
  CHECK(file);

  const InotifyFileToDescriptorMap::iterator it = ifile_map_.find(file);
  if (it == ifile_map_.end()) {
    LOG_INFO << "File has not been registered for change notifications, "
             << "ignoring: " << file->filename();
    return false;
  }

  int wfd = it->second;
  inotify_rm_watch(ifd_, wfd);
  ifile_map_.erase(it);

  const InotifyDescriptorToFileMap::iterator it2 = ifd_map_.find(wfd);
  CHECK(it2 != ifd_map_.end());
  ifd_map_.erase(it2);

  const InotifyFileToHandlerMap::iterator it3 = ihandler_map_.find(file);
  CHECK(it3 != ihandler_map_.end());
  ihandler_map_.erase(it3);

  CHECK_EQ(ifile_map_.size(), ifd_map_.size());
  CHECK_EQ(ifile_map_.size(), ihandler_map_.size());
  return true;
}

#endif

//////////////////////////////////////////////////////////////////////
//
// Kqueue file change notification
//

#if defined(HAVE_KQUEUE)

class KqueueFileChangeSelectable : public Selectable {
 public:
  KqueueFileChangeSelectable(Selector* selector,
                             io::File* file,
                             Selector::FileChangedHandler* handler)
      : Selectable(selector, Selector::kWantMonitor),
        file_(file),
        handler_(handler) {
  }

 private:
  io::File* file_;
  Selector::FileChangedHandler* handler_;

  virtual bool HandleReadEvent(const SelectorEventData& event)
  {
    handler_->Run(selector_, file_, _WatchMaskToEvent(event.internal_event_));
    return true;
  }

  int _WatchMaskToEvent(int fflags) {
    int result = 0;
    if (fflags & NOTE_DELETE) {
      fflags ^= NOTE_DELETE;
      result |= Selector::kFileDeleted;
    }
    if (fflags & (NOTE_WRITE | NOTE_EXTEND)) {
      fflags ^= (NOTE_WRITE | NOTE_EXTEND);
      result |= Selector::kFileModified;
    }
    if (fflags & NOTE_ATTRIB) {
      fflags ^= NOTE_ATTRIB;
      result |= Selector::kFileAttributes;
    }
    if (fflags & NOTE_RENAME) {
      fflags ^= NOTE_RENAME;
      result |= Selector::kFileRenamed;
    }
    if (fflags) {
      result |= Selector::kFileOther;
    }
    return result;
  }

  virtual int GetFd() const { return file_->fd(); }
  virtual void Close() {
    selector_->Unregister(this);
    file_->Close();
  }
};

void Selector::_DestroyKqueueNotifiers()
{
  for (KqFileToSelectableMap::iterator it = kqfile_map_.begin();
       it != kqfile_map_.end();
       ++it) {
    delete it->second;
  }
}

bool Selector::RegisterFileForChangeNotifications(
    io::File* file, Selector::FileChangedHandler* handler)
{
  CHECK(file);
  CHECK(handler && handler->is_permanent());

  const KqFileToSelectableMap::iterator it = kqfile_map_.find(file);
  if (it != kqfile_map_.end()) {
    LOG_ERROR << "File has already been registered for change notifications, "
              << "ignoring: " << file->filename();
    return false;
  }

  KqueueFileChangeSelectable* notifier = new KqueueFileChangeSelectable(
      this, file, handler);
  kqfile_map_.insert(make_pair(file, notifier));
  Register(notifier);
  return true;
}

bool Selector::UnregisterFileForChangeNotifications(io::File* file)
{
  CHECK(file);

  const KqFileToSelectableMap::iterator it = kqfile_map_.find(file);
  if (it == kqfile_map_.end()) {
    LOG_INFO << "File has not been registered for change notifications, "
             << "ignoring: " << file->filename();
    return false;
  }
  KqueueFileChangeSelectable* notifier = it->second;
  Unregister(notifier);
  delete notifier;
  kqfile_map_.erase(it);
  return true;
}

#endif

//////////////////////////////////////////////////////////////////////
}
