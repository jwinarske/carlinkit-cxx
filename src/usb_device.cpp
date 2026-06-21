// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "usb_device.h"

#include <libusb-1.0/libusb.h>

#include <cstdio>
#include <cstdlib>

namespace ck {

std::unique_ptr<UsbDevice> UsbDevice::open(uint16_t vid,
                                           uint16_t pid,
                                           bool quiet,
                                           libusb_context* ext_ctx) {
  const bool owns_ctx = ext_ctx == nullptr;
  libusb_context* ctx = ext_ctx;
  if (owns_ctx && libusb_init(&ctx) != 0) {
    std::fprintf(stderr, "libusb_init failed\n");
    return nullptr;
  }
  libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, vid, pid);
  if (!h) {
    if (!quiet)
      std::fprintf(stderr, "dongle %04x:%04x not found (plugged in?)\n", vid,
                   pid);
    if (owns_ctx)
      libusb_exit(ctx);
    return nullptr;
  }
  libusb_set_auto_detach_kernel_driver(h, 1);
  return std::unique_ptr<UsbDevice>(new UsbDevice(ctx, h, owns_ctx));
}

UsbDevice::~UsbDevice() {
  if (handle_) {
    if (iface_ >= 0)
      libusb_release_interface(handle_, iface_);
    libusb_close(handle_);
  }
  if (ctx_ && owns_ctx_)  // an externally-provided context is the caller's
    libusb_exit(ctx_);
}

bool UsbDevice::claim() {
  libusb_device* dev = libusb_get_device(handle_);

  // NB: do NOT libusb_reset_device() here. The dongle re-enumerates on reset
  // (new device address + node), which both breaks our open handle and drops
  // udev-granted permissions — exactly the "device disappears" hazard
  // node-CarPlay documents. A freshly plugged dongle needs no reset.

  if (int r = libusb_set_configuration(handle_, 1);
      r != 0 && r != LIBUSB_ERROR_BUSY) {
    std::fprintf(stderr, "set_configuration(1) failed: %s\n",
                 libusb_error_name(r));
    return false;
  }

  libusb_config_descriptor* cfg = nullptr;
  if (libusb_get_active_config_descriptor(dev, &cfg) != 0 ||
      cfg->bNumInterfaces == 0) {
    std::fprintf(stderr, "no active config descriptor\n");
    return false;
  }
  // Interface 0 is the vendor-specific CarPlay channel.
  const libusb_interface_descriptor& id = cfg->interface[0].altsetting[0];
  iface_ = id.bInterfaceNumber;
  for (int i = 0; i < id.bNumEndpoints; ++i) {
    const libusb_endpoint_descriptor& ep = id.endpoint[i];
    if ((ep.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK)
      continue;
    if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN)
      ep_in_ = ep.bEndpointAddress;
    else
      ep_out_ = ep.bEndpointAddress;
  }
  libusb_free_config_descriptor(cfg);

  if (!ep_in_ || !ep_out_) {
    std::fprintf(stderr,
                 "could not find bulk IN/OUT endpoints on interface %d\n",
                 iface_);
    return false;
  }
  if (int r = libusb_claim_interface(handle_, iface_); r != 0) {
    std::fprintf(stderr, "claim_interface(%d) failed: %s\n", iface_,
                 libusb_error_name(r));
    return false;
  }
  std::fprintf(stderr, "claimed interface %d  IN=0x%02x OUT=0x%02x\n", iface_,
               ep_in_, ep_out_);
  return true;
}

int UsbDevice::bulk_out(const uint8_t* data, int len, unsigned timeout_ms) {
  int transferred = 0;
  int r = libusb_bulk_transfer(handle_, ep_out_, const_cast<uint8_t*>(data),
                               len, &transferred, timeout_ms);
  if (r != 0)
    return r;
  return transferred;
}

int UsbDevice::bulk_in(uint8_t* data, int len, unsigned timeout_ms) {
  int transferred = 0;
  int r = libusb_bulk_transfer(handle_, ep_in_, data, len, &transferred,
                               timeout_ms);
  if (r != 0 && r != LIBUSB_ERROR_OVERFLOW)
    return r;
  return transferred;
}

uint8_t* UsbDevice::alloc_dma(size_t len) {
  // libusb_dev_mem_alloc returns a zero-copy DMA buffer on Linux when the
  // kernel supports USBDEVFS_MMAP; otherwise nullptr and we fall back.
  if (uint8_t* p = libusb_dev_mem_alloc(handle_, len))
    return p;
  return static_cast<uint8_t*>(std::malloc(len));
}

void UsbDevice::free_dma(uint8_t* p, size_t len) {
  if (!p)
    return;
  if (libusb_dev_mem_free(handle_, p, len) != 0)
    std::free(p);
}

}  // namespace ck
