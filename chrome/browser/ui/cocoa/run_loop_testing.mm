// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/cocoa/run_loop_testing.h"

#import <Foundation/Foundation.h>

#include "base/mac/scoped_nsobject.h"
#include "base/message_loop/message_pump_mac.h"

// This class is scheduled with a delayed selector to quit the message pump.
@interface CocoaQuitTask : NSObject {
 @private
  base::MessagePumpNSRunLoop* pump_;
}
- (id)initWithMessagePump:(base::MessagePumpNSRunLoop*)pump;
- (void)doQuit;
@end

@implementation CocoaQuitTask
- (id)initWithMessagePump:(base::MessagePumpNSRunLoop*)pump {
  if ((self = [super init])) {
    pump_ = pump;
  }
  return self;
}

- (void)doQuit {
  pump_->Quit();
}
@end

////////////////////////////////////////////////////////////////////////////////

namespace chrome {
namespace testing {

void NSRunLoopRunAllPending() {
  scoped_refptr<base::MessagePumpNSRunLoop> message_pump(
      new base::MessagePumpNSRunLoop);

  // Put a delayed selector on the queue. All other pending delayed selectors
  // will run before this, after which the internal loop can end.
  base::scoped_nsobject<CocoaQuitTask> quit_task(
      [[CocoaQuitTask alloc] initWithMessagePump:message_pump]);

  [quit_task performSelector:@selector(doQuit)
                  withObject:nil
                  afterDelay:0];

  // Spin the internal loop, running it until the quit task is pumped. Pass NULL
  // because there is no delegate MessageLoop; only the Cocoa work queues will
  // be pumped.
  message_pump->Run(NULL);
}

}  // namespace testing
}  // namespace chrome
