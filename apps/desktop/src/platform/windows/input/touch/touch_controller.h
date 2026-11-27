/*
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _TOUCH_CONTROLLER_H_
#define _TOUCH_CONTROLLER_H_

#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

#include <remote_action.h>

#include "display_info.h"

namespace crossdesk {

// Injects the controller's fingers as native Windows touch.
//
// Injecting touch instead of synthesising mouse events is what lets the host's
// own gesture engine interpret the input: a swipe scrolls, a pinch zooms and a
// press-and-hold opens the context menu, exactly as on a touch-screen PC. The
// trade-off is that only applications that understand touch react to it, so
// callers keep the mouse path as a fallback.
//
// Requires Windows 10 1809 or newer (CreateSyntheticPointerDevice).
class TouchController {
 public:
  TouchController();
  ~TouchController();

  TouchController(const TouchController&) = delete;
  TouchController& operator=(const TouchController&) = delete;

  int Init();
  int Destroy();

  // Applies one full snapshot: contacts missing from `touch` are lifted,
  // new ones are pressed, the rest move. Returns 0 on success.
  int SendTouchCommand(const Touch& touch,
                       const std::vector<DisplayInfo>& displays,
                       int display_index);

  // Lifts every contact, e.g. when the controller disconnects.
  void ReleaseAll();

 private:
  void ReleaseAllLocked();

  HSYNTHETICPOINTERDEVICE device_ = nullptr;
  std::map<int, POINT> active_;
  std::mutex mutex_;
};

}  // namespace crossdesk

#endif
