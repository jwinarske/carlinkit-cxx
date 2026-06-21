// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>

struct libusb_context;
struct libusb_device_handle;

namespace ck {

// Default Carlinkit "Auto Box" identity.
constexpr uint16_t kDongleVid = 0x1314;
constexpr uint16_t kDonglePid = 0x1520;  // 0x1521 also seen

// RAII libusb wrapper specialised for the dongle's single vendor interface.
// Provides synchronous bulk I/O. Bulk-IN buffers should be obtained from
// alloc_dma()/free_dma() so libusb can use zero-copy DMA (USBDEVFS_MMAP)
// where the kernel supports it, falling back to plain malloc transparently.
class UsbDevice {
 public:
  // quiet suppresses the "not found" message (the supervisor polls open()).
  // ext_ctx, when non-null, is an existing libusb context to open on (the
  // caller retains ownership and must outlive the device); the supervisor
  // passes one so repeated polling doesn't init/exit a context each time. When
  // null, a private context is created and owned by the returned device.
  static std::unique_ptr<UsbDevice> open(uint16_t vid = kDongleVid,
                                         uint16_t pid = kDonglePid,
                                         bool quiet = false,
                                         libusb_context* ext_ctx = nullptr);
  ~UsbDevice();
  UsbDevice(const UsbDevice&) = delete;
  UsbDevice& operator=(const UsbDevice&) = delete;

  // Reset, select configuration 1, claim interface 0, and discover the
  // vendor bulk IN/OUT endpoints. Returns false on any failure.
  bool claim();

  // Synchronous bulk transfer. Returns bytes transferred, or <0 libusb error.
  int bulk_out(const uint8_t* data, int len, unsigned timeout_ms = 1000);
  int bulk_in(uint8_t* data,
              int len,
              unsigned timeout_ms = 0);  // 0 => no timeout

  // Zero-copy-capable DMA buffer for bulk-IN (USBDEVFS_MMAP). Falls back to
  // malloc if the platform lacks support. Free with free_dma().
  uint8_t* alloc_dma(size_t len);
  void free_dma(uint8_t* p, size_t len);

  uint8_t ep_in() const { return ep_in_; }
  uint8_t ep_out() const { return ep_out_; }
  libusb_device_handle* handle() const { return handle_; }
  libusb_context* context() const { return ctx_; }

 private:
  UsbDevice(libusb_context* ctx, libusb_device_handle* h, bool owns_ctx)
      : ctx_(ctx), handle_(h), owns_ctx_(owns_ctx) {}
  libusb_context* ctx_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  bool owns_ctx_ = true;
  int iface_ = -1;
  uint8_t ep_in_ = 0;
  uint8_t ep_out_ = 0;
};

}  // namespace ck
