/*
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "touch_controller.h"

#include <algorithm>

#include "rd_log.h"

namespace crossdesk {
namespace {

constexpr int kMaxContacts = 10;
// Windows drops contacts it stops hearing about. Re-sending the snapshot keeps
// a finger that rests still alive without generating a movement.
constexpr LONG kContactDiameter = 4;
constexpr UINT32 kContactPressure = 512;

// The layouts below mirror POINTER_INFO / POINTER_TOUCH_INFO /
// POINTER_TYPE_INFO. A mismatch would corrupt the injected frames, so the
// sizes are checked at compile time instead of trusting the field order.
#if defined(_WIN64)
static_assert(sizeof(SyntheticPointerInfo) == 96,
              "POINTER_INFO layout changed");
static_assert(sizeof(SyntheticTouchInfo) == 144,
              "POINTER_TOUCH_INFO layout changed");
static_assert(sizeof(SyntheticPointerTypeInfo) == 152,
              "POINTER_TYPE_INFO layout changed");
#else
static_assert(sizeof(SyntheticPointerInfo) == 88,
              "POINTER_INFO layout changed");
static_assert(sizeof(SyntheticTouchInfo) == 136,
              "POINTER_TOUCH_INFO layout changed");
static_assert(sizeof(SyntheticPointerTypeInfo) == 144,
              "POINTER_TYPE_INFO layout changed");
#endif

void FillTouchInfo(SyntheticTouchInfo* touch, UINT32 pointer_id,
                   UINT32 flags, POINT pixel) {
  touch->pointerInfo.pointerType = kPointerTypeTouch;
  touch->pointerInfo.pointerId = pointer_id;
  touch->pointerInfo.pointerFlags = flags;
  touch->pointerInfo.ptPixelLocation = pixel;
  touch->pointerInfo.ButtonChangeType = 0;
  touch->touchFlags = 0;
  touch->touchMask = kTouchMaskContactArea | kTouchMaskPressure;
  touch->rcContact = {pixel.x - kContactDiameter / 2,
                      pixel.y - kContactDiameter / 2,
                      pixel.x + kContactDiameter / 2,
                      pixel.y + kContactDiameter / 2};
  touch->pressure = kContactPressure;
}

bool ToPixel(const std::vector<DisplayInfo>& displays, int display_index,
             float normalized_x, float normalized_y, POINT* out) {
  if (out == nullptr || display_index < 0 ||
      display_index >= static_cast<int>(displays.size()))
    return false;
  const DisplayInfo& display = displays[display_index];
  if (display.width <= 0 || display.height <= 0) return false;
  const float x = std::clamp(normalized_x, 0.0f, 1.0f);
  const float y = std::clamp(normalized_y, 0.0f, 1.0f);
  out->x = static_cast<LONG>(x * display.width) + display.left;
  out->y = static_cast<LONG>(y * display.height) + display.top;
  return true;
}

}  // namespace

TouchController::TouchController() = default;

TouchController::~TouchController() { Destroy(); }

int TouchController::Init() {
  std::lock_guard<std::mutex> lock(mutex_);
  return EnsureDeviceLocked();
}

int TouchController::EnsureDeviceLocked() {
  if (device_ != nullptr) return 0;
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) return -1;
  if (create_device_ == nullptr) {
    create_device_ = reinterpret_cast<CreateSyntheticPointerDeviceFn>(
        GetProcAddress(user32, "CreateSyntheticPointerDevice"));
    inject_ = reinterpret_cast<InjectSyntheticPointerInputFn>(
        GetProcAddress(user32, "InjectSyntheticPointerInput"));
    destroy_device_ = reinterpret_cast<DestroySyntheticPointerDeviceFn>(
        GetProcAddress(user32, "DestroySyntheticPointerDevice"));
  }
  if (create_device_ == nullptr || inject_ == nullptr ||
      destroy_device_ == nullptr) {
    LOG_WARN("Touch injection unavailable on this Windows version");
    return -1;
  }
  device_ = create_device_(kPointerTypeTouch,
                           static_cast<ULONG>(kMaxContacts),
                           kPointerFeedbackDefault);
  if (device_ == nullptr) {
    LOG_WARN("Touch injection device creation failed, err={}", GetLastError());
    return -1;
  }
  LOG_INFO("Touch injection ready: max_contacts={}", kMaxContacts);
  return 0;
}

int TouchController::Destroy() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAllLocked();
  if (device_ != nullptr && destroy_device_ != nullptr) {
    destroy_device_(device_);
  }
  device_ = nullptr;
  return 0;
}

void TouchController::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAllLocked();
}

void TouchController::ReleaseAllLocked() {
  if (active_.empty() || device_ == nullptr || inject_ == nullptr) {
    active_.clear();
    return;
  }
  std::vector<SyntheticPointerTypeInfo> infos;
  infos.reserve(active_.size());
  for (const auto& entry : active_) {
    SyntheticPointerTypeInfo info{};
    info.type = kPointerTypeTouch;
    FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first),
                  kPointerFlagUp | kPointerFlagInRange, entry.second);
    infos.push_back(info);
  }
  inject_(device_, infos.data(), static_cast<UINT32>(infos.size()));
  active_.clear();
}

int TouchController::SendTouchCommand(const Touch& touch,
                                      const std::vector<DisplayInfo>& displays,
                                      int display_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (EnsureDeviceLocked() != 0) return -1;
  if (touch.count > kMaxTouchPoints) return -1;

  std::map<int, POINT> next;
  for (std::size_t index = 0; index < touch.count; ++index) {
    const TouchPoint& point = touch.points[index];
    if (point.id < 0 || point.id >= kMaxContacts) return -1;
    POINT pixel{};
    if (!ToPixel(displays, display_index, point.x, point.y, &pixel))
      return -1;
    next[point.id] = pixel;
  }

  std::vector<SyntheticPointerTypeInfo> infos;
  infos.reserve(next.size() + active_.size());

  // Lift contacts that disappeared from the snapshot first, so a subsequent
  // press with the same id starts a fresh contact instead of continuing one.
  for (const auto& entry : active_) {
    if (next.count(entry.first) != 0) continue;
    SyntheticPointerTypeInfo info{};
    info.type = kPointerTypeTouch;
    FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first),
                  kPointerFlagUp | kPointerFlagInRange, entry.second);
    infos.push_back(info);
  }

  bool has_primary = false;
  for (const auto& entry : next) {
    SyntheticPointerTypeInfo info{};
    info.type = kPointerTypeTouch;
    UINT32 flags = kPointerFlagInRange | kPointerFlagInContact;
    if (active_.count(entry.first) == 0) {
      flags |= kPointerFlagDown;
    } else {
      // Unchanged contacts are re-sent too: Windows cancels a contact that
      // stops reporting.
      flags |= kPointerFlagUpdate;
    }
    if (!has_primary) {
      flags |= kPointerFlagPrimary;
      has_primary = true;
    }
    FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first), flags,
                  entry.second);
    infos.push_back(info);
  }

  if (!infos.empty() &&
      !inject_(device_, infos.data(), static_cast<UINT32>(infos.size()))) {
    // Do not remember a state the host never received: the next snapshot
    // rebuilds every contact instead of sending corrections against contacts
    // Windows has no record of.
    static uint32_t fail_log_counter = 0;
    if (fail_log_counter++ % 120 == 0) {
      LOG_WARN("Touch injection failed, err={}, contacts={}", GetLastError(),
               infos.size());
    }
    active_.clear();
    return -1;
  }

  active_.swap(next);
  return 0;
}

}  // namespace crossdesk
