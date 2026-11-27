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

// The synthetic pointer API shipped with Windows 10 1809, but the SDK only
// declares it when the headers are built for that target. Builds that stay on
// a lower default surface hide HSYNTHETICPOINTERDEVICE entirely, so the wire
// structures and the entry points are declared here and resolved from user32
// at runtime. Only plain Win32 types are used, which keeps this independent of
// whichever SDK surface the toolchain selects.
using SyntheticPointerDevice = HANDLE;

struct SyntheticPointerInfo {
  UINT32 pointerType;
  UINT32 pointerId;
  UINT32 frameId;
  UINT32 pointerFlags;
  HANDLE sourceDevice;
  HWND hwndTarget;
  POINT ptPixelLocation;
  POINT ptHimetricLocation;
  POINT ptPixelLocationRaw;
  POINT ptHimetricLocationRaw;
  DWORD dwTime;
  UINT32 historyCount;
  INT32 InputData;
  UINT32 dwKeyStates;
  UINT64 PerformanceCount;
  UINT32 ButtonChangeType;
};

struct SyntheticTouchInfo {
  SyntheticPointerInfo pointerInfo;
  UINT32 touchFlags;
  UINT32 touchMask;
  RECT rcContact;
  RECT rcContactRaw;
  UINT32 orientation;
  UINT32 pressure;
};

struct SyntheticPointerTypeInfo {
  UINT32 type;
#if defined(_WIN64)
  UINT32 reserved;
#endif
  SyntheticTouchInfo touchInfo;
};

// Values from winuser.h. They are part of the ABI and do not move.
constexpr UINT32 kPointerTypeTouch = 2;
constexpr UINT32 kPointerFlagInRange = 0x00000002;
constexpr UINT32 kPointerFlagInContact = 0x00000004;
constexpr UINT32 kPointerFlagPrimary = 0x00002000;
constexpr UINT32 kPointerFlagDown = 0x00010000;
constexpr UINT32 kPointerFlagUpdate = 0x00020000;
constexpr UINT32 kPointerFlagUp = 0x00040000;
constexpr UINT32 kTouchMaskContactArea = 0x00000001;
constexpr UINT32 kTouchMaskPressure = 0x00000004;
constexpr UINT32 kPointerFeedbackDefault = 1;

// Injects the controller's fingers as native Windows touch.
//
// Injecting touch instead of synthesising mouse events is what lets the host's
// own gesture engine interpret the input: a swipe scrolls, a pinch zooms and a
// press-and-hold opens the context menu, exactly as on a touch-screen PC. The
// trade-off is that only applications that understand touch react to it, so
// callers keep the mouse path as a fallback.
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
  using CreateSyntheticPointerDeviceFn = SyntheticPointerDevice(WINAPI*)(
      UINT32 pointer_type, ULONG max_count, UINT32 mode);
  using InjectSyntheticPointerInputFn =
      BOOL(WINAPI*)(SyntheticPointerDevice device,
                    const SyntheticPointerTypeInfo* pointer_info, UINT32 count);
  using DestroySyntheticPointerDeviceFn =
      VOID(WINAPI*)(SyntheticPointerDevice device);

  void ReleaseAllLocked();
  int EnsureDeviceLocked();

  SyntheticPointerDevice device_ = nullptr;
  CreateSyntheticPointerDeviceFn create_device_ = nullptr;
  InjectSyntheticPointerInputFn inject_ = nullptr;
  DestroySyntheticPointerDeviceFn destroy_device_ = nullptr;
  std::map<int, POINT> active_;
  std::mutex mutex_;
};

}  // namespace crossdesk

#endif
