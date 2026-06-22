// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Translate drm-cxx libinput events into Carlinkit touch messages.
//
// The video occupies a sub-rectangle (vx,vy,vw,vh) of the display (dw,dh).
// Touch coordinates arrive normalized to the whole display ([0,1]); we remap
// them into the video rect's local [0,1] space (the dongle's touch coordinate
// system) and drop touches outside the video. A pointer (mouse) is supported
// too: relative motion drives a cursor, left button is touch down/up.
#include <linux/input-event-codes.h>
#include <drm-cxx/input/seat.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <variant>

#include "protocol.h"

namespace ck {

class TouchMapper {
 public:
  using SendFn = std::function<void(float x, float y, TouchAction action)>;

  // rot_deg is the display rotation (0/90/180/270). The video rect
  // (vx,vy,vw,vh) is in screen space (its extent already swapped for 90/270);
  // touches mapped into it are un-rotated back to the source frame in to_video.
  TouchMapper(int dw,
              int dh,
              int vx,
              int vy,
              int vw,
              int vh,
              SendFn send,
              int rot_deg = 0)
      : dw_(dw),
        dh_(dh),
        vx_(vx),
        vy_(vy),
        vw_(vw),
        vh_(vh),
        send_(std::move(send)),
        rot_deg_(rot_deg),
        cx_(dw / 2.0),
        cy_(dh / 2.0) {}

  [[nodiscard]] bool quit() const { return quit_; }

  // Pointer speed multiplier applied to relative mouse motion (the bare
  // libinput deltas feel slow on a large display).
  void set_pointer_gain(double g) { gain_ = g; }
  // Separate, usually higher multiplier used while a button is held (a drag /
  // swipe): the precise free-cursor gain is too slow to fling lists, and a
  // mouse drag is bounded by the cursor reaching the screen edge.
  void set_drag_gain(double g) { drag_gain_ = g; }
  // Touch report rate while dragging (Hz). CarPlay's fling detector expects
  // touchscreen-like rates; a raw ~1000 Hz mouse stream collapses its velocity
  // estimate and kills momentum, so drag motion is coalesced to this rate.
  void set_touch_rate_hz(double hz) {
    if (hz > 0.0)
      touch_interval_ =
          std::chrono::microseconds(static_cast<long>(1.0e6 / hz));
  }

  // Coalesced cursor position for the app to drive a HW cursor once per frame
  // (rather than committing the cursor plane on every motion event). These are
  // read from the render thread while handle() runs on the input thread, so the
  // shared cursor state is mutex-guarded.
  [[nodiscard]] int cursor_x() const {
    std::lock_guard<std::mutex> lk(cur_m_);
    return static_cast<int>(cx_);
  }
  [[nodiscard]] int cursor_y() const {
    std::lock_guard<std::mutex> lk(cur_m_);
    return static_cast<int>(cy_);
  }
  bool take_cursor_dirty() {
    std::lock_guard<std::mutex> lk(cur_m_);
    const bool d = cursor_dirty_;
    cursor_dirty_ = false;
    return d;
  }

  void handle(const drm::input::InputEvent& ev) {
    if (const auto* ke = std::get_if<drm::input::KeyboardEvent>(&ev)) {
      if (ke->pressed && (ke->key == KEY_ESC || ke->key == KEY_Q))
        quit_ = true;
      return;
    }
    if (const auto* pe = std::get_if<drm::input::PointerEvent>(&ev)) {
      handle_pointer(*pe);
      return;
    }
    if (const auto* te = std::get_if<drm::input::TouchEvent>(&ev)) {
      handle_touch(*te);
    }
  }

 private:
  // Map display-normalized [0,1] to the video rect's local [0,1]; false if
  // outside the video.
  bool to_video(double ndx, double ndy, float& ox, float& oy) const {
    const double lx = (ndx * dw_ - vx_) / vw_;
    const double ly = (ndy * dh_ - vy_) / vh_;
    if (lx < 0.0 || lx > 1.0 || ly < 0.0 || ly > 1.0)
      return false;
    // (lx,ly) is within the on-screen content rect, which holds the rotated
    // image. Apply the inverse rotation to recover the coordinate in the
    // unrotated video frame — the coordinate system the dongle expects.
    double u = lx;
    double v = ly;
    switch (rot_deg_) {
      case 90:
        u = 1.0 - ly;
        v = lx;
        break;
      case 180:
        u = 1.0 - lx;
        v = 1.0 - ly;
        break;
      case 270:
        u = ly;
        v = 1.0 - lx;
        break;
      default:  // 0 — identity
        break;
    }
    ox = static_cast<float>(u);
    oy = static_cast<float>(v);
    return true;
  }

  // Rotate a relative motion delta from the input frame into the display frame
  // so the cursor tracks the rotated content (mouse-right moves the pointer
  // along the content's rightward axis). The forward rotation here is the
  // inverse of to_video's, so a drag's position still maps back correctly.
  void rotate_delta(double& dx, double& dy) const {
    const double x = dx;
    const double y = dy;
    switch (rot_deg_) {
      case 90:
        dx = y;
        dy = -x;
        break;
      case 180:
        dx = -x;
        dy = -y;
        break;
      case 270:
        dx = -y;
        dy = x;
        break;
      default:  // 0 — identity
        break;
    }
  }

  void emit(TouchAction action, double ndx, double ndy) {
    float x = 0, y = 0;
    if (!to_video(ndx, ndy, x, y)) {
      if (action != TouchAction::Up)
        return;  // ignore presses/moves outside the video area
      x = last_x_;
      y = last_y_;  // release at the last in-bounds point
    } else {
      last_x_ = x;
      last_y_ = y;
    }
    send_(x, y, action);
  }

  void handle_touch(const drm::input::TouchEvent& te) {
    using T = drm::input::TouchEvent::Type;
    switch (te.type) {
      case T::Down:
        down_ = true;
        emit(TouchAction::Down, te.x, te.y);
        break;
      case T::Motion:
        if (down_)
          emit(TouchAction::Move, te.x, te.y);
        break;
      case T::Up:
      case T::Cancel:
        // libinput touch-up carries no coords; release at the last position.
        if (down_)
          send_(last_x_, last_y_, TouchAction::Up);
        down_ = false;
        break;
      default:  // Frame
        break;
    }
  }

  void handle_pointer(const drm::input::PointerEvent& pe) {
    if (const auto* m = std::get_if<drm::input::PointerMotionEvent>(&pe)) {
      double lx = 0, ly = 0;
      double ddx = m->dx, ddy = m->dy;
      rotate_delta(ddx, ddy);  // align motion with the rotated display
      {
        std::lock_guard<std::mutex> lk(cur_m_);
        const double g = down_ ? drag_gain_ : gain_;  // faster while dragging
        cx_ = std::clamp(cx_ + ddx * g, 0.0, static_cast<double>(dw_));
        cy_ = std::clamp(cy_ + ddy * g, 0.0, static_cast<double>(dh_));
        cursor_dirty_ = true;
        lx = cx_;
        ly = cy_;
      }
      if (down_) {
        // Coalesce drag motion to a touchscreen-like report rate so CarPlay
        // sees finger-like velocity and registers momentum on release. The
        // cursor itself still updated every event above, so it stays smooth.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_move_ >= touch_interval_) {
          last_move_ = now;
          emit(TouchAction::Move, lx / dw_, ly / dh_);
        }
      }
    } else if (const auto* b =
                   std::get_if<drm::input::PointerButtonEvent>(&pe)) {
      if (b->button != BTN_LEFT)
        return;
      down_ = b->pressed;
      if (b->pressed)  // let the first drag move send immediately
        last_move_ = std::chrono::steady_clock::time_point{};
      emit(b->pressed ? TouchAction::Down : TouchAction::Up, cx_ / dw_,
           cy_ / dh_);
    }
  }

  int dw_, dh_, vx_, vy_, vw_, vh_;
  SendFn send_;
  int rot_deg_ = 0;  // display rotation; un-rotates touches in to_video
  double gain_ = 2.5;
  double drag_gain_ =
      5.0;  // while a button is held; tune via CARLINKIT_DRAG_GAIN
  float last_x_ = 0.5F, last_y_ = 0.5F;
  bool down_ = false;  // input-thread only
  // Drag-motion coalescing (input-thread only). ~90 Hz mimics a touchscreen.
  std::chrono::steady_clock::time_point last_move_{};
  std::chrono::microseconds touch_interval_{11111};
  std::atomic<bool> quit_{false};

  // Shared cursor state: written by handle() on the input thread, read by the
  // render thread via cursor_x()/cursor_y()/take_cursor_dirty().
  mutable std::mutex cur_m_;
  double cx_, cy_;  // pointer cursor in display pixels
  bool cursor_dirty_ = false;
};

}  // namespace ck
