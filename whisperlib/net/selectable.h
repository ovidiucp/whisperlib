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
// Author: Cosmin Tudorache & Catalin Popescu
//
#ifndef __NET_BASE_SELECTABLE_H__
#define __NET_BASE_SELECTABLE_H__

#include <whisperlib/io/buffer/memory_stream.h>
#include <whisperlib/net/selector.h>
#include <whisperlib/net/selector_event_data.h>

namespace net {

class Selectable {
 public:
  Selectable()
      : selector_(NULL),
        desire_(Selector::kWantRead | Selector::kWantError) {
  }
  explicit Selectable(Selector* selector)
      : selector_(selector),
        desire_(Selector::kWantRead | Selector::kWantError) {
  }
  explicit Selectable(Selector* selector, int desire)
      : selector_(selector),
        desire_(desire) {
  }

  virtual ~Selectable() {
  }

  Selector* selector() const {
    return selector_;
  }
  void set_selector(Selector* selector) {
    CHECK(selector_ == NULL || selector == NULL);
    selector_ = selector;
  }

  // In any of the following events the obj is safe to close itself.
  // The selector will notice the closure (by checking IsOpen()),
  // and will remove this obj from it's queue.

  // Signal that informs the selectable object that it should read from
  // its registered fd.
  // Return true if the events should be contiued to be processesd w/ respect
  // to this selectable object.
  virtual bool HandleReadEvent(const SelectorEventData& event) {
    return true;
  }

  // Signal that informs the selectable object that it can write data out
  // Return true if the events should be contiued to be processesd w/ respect
  // to this selectable object.
  virtual bool HandleWriteEvent(const SelectorEventData& event) {
    return true;
  }

  // Signal an error(exception) occured on the fd.
  // Return true if the events should be contiued to be processesd w/ respect
  // to this selectable object.
  virtual bool HandleErrorEvent(const SelectorEventData& event) {
    return true;
  }

  // Returns the file descriptor associated w/ this Selectable object (if any)
  virtual int GetFd() const = 0;

  virtual void Close() = 0;

 protected:
  // Read/Write basics
  int32 Write(const char* buf, int32 size);
  int32 Read(char* buf, int32 size);

  // Read/Write interface for MemoryStream-s for copy-less stuff
  int32 Write(io::MemoryStream* ms, int32 size = -1);
  int32 Read(io::MemoryStream* ms, int32 size = -1);

  // the selector that controls this object
  Selector* selector_;

 private:
  // the desire for read or write **DO NOT TOUCH** updated by the selector only
  int32 desire_;

  friend class Selector;
};
}

#endif  // __NET_BASE_SELECTABLE_H__
