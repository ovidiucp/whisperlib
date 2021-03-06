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
// Author: Catalin Popescu

#include "whisperlib/sync/thread_pool.h"
#include "whisperlib/base/strutil.h"

namespace thread {

ThreadPool::ThreadPool(uint32 pool_size, uint32 backlog_size, bool low_priority)
  : jobs_(backlog_size),
    count_completed_(pool_size, 0) {
  CHECK_GT(backlog_size, pool_size);
  for ( uint32 i = 0; i < pool_size; ++i ) {
    threads_.push_back(new Thread(NewCallback(this, &ThreadPool::ThreadRun, i), low_priority));
    threads_[i]->SetJoinable();
    threads_[i]->Start();
  }
}


ThreadPool::~ThreadPool() {
  Closure* callback;
  do {
    callback = jobs_.Get(0);
    if ( callback != NULL && !callback->is_permanent() ) {
      delete callback;
    }
  } while ( callback != NULL );
  for ( uint32 i = 0; i < threads_.size(); ++i ) {
    while ( !jobs_.Put(NULL, 0) ) {
      callback = jobs_.Get(0);
      if ( callback != NULL && !callback->is_permanent() ) {
        delete callback;
      }
    }
  }
  for ( uint32 i = 0; i < threads_.size(); ++i ) {
    threads_[i]->Join();
    delete threads_[i];
  }
  LOG_INFO << "ThreadPool jobs completed by each thread: " << strutil::ToString(count_completed_);
}

void ThreadPool::FinishWork() {
  for ( uint32 i = 0; i < threads_.size(); ++i ) {
    jobs_.Put(NULL);
  }
  for ( uint32 i = 0; i < threads_.size(); ++i ) {
    threads_[i]->Join();
    delete threads_[i];
  }
  threads_.clear();
}


void ThreadPool::ThreadRun(uint32 thread_index) {
  while ( true ) {
    Closure* callback = jobs_.Get();
    if ( callback == NULL ) {
      return;
    }
    callback->Run();
    count_completed_[thread_index]++;
  }
}
}
