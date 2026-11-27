/*
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "touch_controller.h"

#include <algorithm>

#include "log.h"

namespace crossdesk {
namespace {

constexpr int kMaxContacts = 10;
// Windows drops contacts it stops hearing about. Re-sending the snapshot keeps
// a finger that rests still alive without generating a movement.
constexpr USHORT kContactDiameter = 4;
constexpr UINT32 kContactPressure = 512;

void FillTouchInfo(POINTER_TOUCH_INFO* touch, UINT32 pointer_id,
                   POINTER_FLAGS flags, POINT pixel) {
  touch->pointerInfo.pointerType = PT_TOUCH;
  touch->pointerInfo.pointerId = pointer_id;
  touch->pointerInfo.pointerFlags = flags;
  touch->pointerInfo.ptPixelLocation = pixel;
  touch->pointerInfo.ButtonChangeType = POINTER_CHANGE_NONE;
  touch->touchFlags = TOUCH_FLAG_NONE;
  touch->touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
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
  if (device_ != nullptr) return 0;
  device_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts,
                                         POINTER_FEEDBACK_DEFAULT);
  if (device_ == nullptr) {
    LOG_WARN("Touch injection unavailable, err={}", GetLastError());
    return -1;
  }
  return 0;
}

int TouchController::Destroy() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAllLocked();
  if (device_ != nullptr) {
    DestroySyntheticPointerDevice(device_);
    device_ = nullptr;
  }
  return 0;
}

void TouchController::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReleaseAllLocked();
}

void TouchController::ReleaseAllLocked() {
  if (active_.empty()) return;
  if (device_ != nullptr) {
    std::vector<POINTER_TYPE_INFO> infos;
    infos.reserve(active_.size());
    for (const auto& entry : active_) {
      POINTER_TYPE_INFO info{};
      info.type = PT_TOUCH;
      FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first),
                    static_cast<POINTER_FLAGS>(POINTER_FLAG_UP |
                                               POINTER_FLAG_INRANGE),
                    entry.second);
      infos.push_back(info);
    }
    InjectSyntheticPointerInput(device_, infos.data(),
                                static_cast<UINT32>(infos.size()));
  }
  active_.clear();
}

int TouchController::SendTouchCommand(const Touch& touch,
                                      const std::vector<DisplayInfo>& displays,
                                      int display_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (device_ == nullptr) {
    // Init() takes the same mutex, so create the device inline here.
    device_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts,
                                           POINTER_FEEDBACK_DEFAULT);
    if (device_ == nullptr) {
      LOG_WARN("Touch injection unavailable, err={}", GetLastError());
      return -1;
    }
  }
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

  std::vector<POINTER_TYPE_INFO> infos;
  infos.reserve(next.size() + active_.size());

  // Lift contacts that disappeared from the snapshot first, so a subsequent
  // press with the same id starts a fresh contact instead of continuing one.
  for (const auto& entry : active_) {
    if (next.count(entry.first) != 0) continue;
    POINTER_TYPE_INFO info{};
    info.type = PT_TOUCH;
    FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first),
                  static_cast<POINTER_FLAGS>(POINTER_FLAG_UP |
                                             POINTER_FLAG_INRANGE),
                  entry.second);
    infos.push_back(info);
  }

  bool has_primary = false;
  for (const auto& entry : next) {
    POINTER_TYPE_INFO info{};
    info.type = PT_TOUCH;
    POINTER_FLAGS flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    if (active_.count(entry.first) == 0) {
      flags = static_cast<POINTER_FLAGS>(flags | POINTER_FLAG_DOWN);
    } else if (active_.at(entry.first).x == entry.second.x &&
               active_.at(entry.first).y == entry.second.y) {
      // Unchanged contact: still re-sent so Windows keeps it alive.
      flags = static_cast<POINTER_FLAGS>(flags | POINTER_FLAG_UPDATE);
    } else {
      flags = static_cast<POINTER_FLAGS>(flags | POINTER_FLAG_UPDATE);
    }
    if (!has_primary) {
      flags = static_cast<POINTER_FLAGS>(flags | POINTER_FLAG_PRIMARY);
      has_primary = true;
    }
    FillTouchInfo(&info.touchInfo, static_cast<UINT32>(entry.first), flags,
                  entry.second);
    infos.push_back(info);
  }

  if (!infos.empty() &&
      !InjectSyntheticPointerInput(device_, infos.data(),
                                   static_cast<UINT32>(infos.size()))) {
    LOG_WARN("Touch injection failed, err={}, contacts={}", GetLastError(),
             infos.size());
    active_.swap(next);
    return -1;
  }

  active_.swap(next);
  return 0;
}

}  // namespace crossdesk
