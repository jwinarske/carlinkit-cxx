// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Carlinkit "Auto Box" (1314:1520/1521) framed bulk protocol.
// Ported from rhysmorgan134/node-CarPlay (src/modules/messages). All multi-byte
// fields are little-endian. Every message on the wire is a 16-byte header
// optionally followed by a payload of `length` bytes.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ck {

constexpr uint32_t kMagic = 0x55aa55aa;
constexpr size_t kHeaderLen = 16;

enum class MessageType : uint32_t {
  Open = 0x01,
  Plugged = 0x02,
  Phase = 0x03,
  Unplugged = 0x04,
  Touch = 0x05,
  VideoData = 0x06,
  AudioData = 0x07,
  Command = 0x08,
  LogoType = 0x09,
  BluetoothAddress = 0x0a,
  BluetoothPIN = 0x0c,
  BluetoothDeviceName = 0x0d,
  WifiDeviceName = 0x0e,
  DisconnectPhone = 0x0f,
  BluetoothPairedList = 0x12,
  ManufacturerInfo = 0x14,
  CloseDongle = 0x15,
  MultiTouch = 0x17,
  HiCarLink = 0x18,
  BoxSettings = 0x19,
  MediaData = 0x2a,
  SendFile = 0x99,
  HeartBeat = 0xaa,
  SoftwareVersion = 0xcc,
};

// Command values (MessageType::Command payload, u32). Subset we use + notable
// ones.
enum class Command : uint32_t {
  Invalid = 0,
  StartRecordAudio = 1,
  StopRecordAudio = 2,
  RequestHostUI = 3,
  Siri = 5,
  Mic = 7,
  BoxMic = 15,
  EnableNightMode = 16,
  DisableNightMode = 17,
  AudioTransferOn = 22,
  AudioTransferOff = 23,
  Wifi24g = 24,
  Wifi5g = 25,
  Frame = 12,
  AcceptPhone = 300,
  RejectPhone = 301,
  RequestVideoFocus = 500,
  ReleaseVideoFocus = 501,
  WifiEnable = 1000,
  AutoConnectEnable = 1001,
  WifiConnect = 1002,
  ScanningDevice = 1003,
  DeviceFound = 1004,
  DeviceNotFound = 1005,
  ConnectDeviceFailed = 1006,
  BtConnected = 1007,
  BtDisconnected = 1008,
  WifiConnected = 1009,
  WifiDisconnected = 1010,
  BtPairStart = 1011,
  WifiPair = 1012,
};

enum class PhoneType : uint32_t {
  AndroidMirror = 1,
  CarPlay = 3,
  iPhoneMirror = 4,
  AndroidAuto = 5,
  HiCar = 6,
};

enum class TouchAction : uint32_t { Down = 14, Move = 15, Up = 16 };

// AudioData control sub-commands (1-byte payload after the 12-byte audio
// header).
enum class AudioCommand : int {
  OutputStart = 1,
  OutputStop = 2,
  InputConfig = 3,
  PhonecallStart = 4,
  PhonecallStop = 5,
  NaviStart = 6,
  NaviStop = 7,
  SiriStart = 8,
  SiriStop = 9,
  MediaStart = 10,
  MediaStop = 11,
  AlertStart = 12,
  AlertStop = 13,
};

// Dongle "files" written via SendFile; the path selects the setting.
namespace file {
constexpr const char* kDpi = "/tmp/screen_dpi";
constexpr const char* kNightMode = "/tmp/night_mode";
constexpr const char* kHandDrive = "/tmp/hand_drive_mode";
constexpr const char* kChargeMode = "/tmp/charge_mode";
constexpr const char* kBoxName = "/etc/box_name";
constexpr const char* kAndroidWorkMode = "/etc/android_work_mode";
}  // namespace file

struct DongleConfig {
  uint32_t width = 800;
  uint32_t height = 640;
  uint32_t fps = 20;
  uint32_t dpi = 160;
  uint32_t format = 5;
  uint32_t iBoxVersion = 2;
  uint32_t packetMax = 49152;
  uint32_t phoneWorkMode = 2;
  uint32_t hand = 0;  // 0=LHD 1=RHD
  uint32_t mediaDelay = 300;
  bool nightMode = false;
  bool audioTransferMode = false;
  bool wifi5g = true;
  bool boxMic = false;  // false => OS mic
  std::string boxName = "nodePlay";
};

// ── little-endian helpers ────────────────────────────────────────────────
inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(v & 0xff);
  b.push_back((v >> 8) & 0xff);
  b.push_back((v >> 16) & 0xff);
  b.push_back((v >> 24) & 0xff);
}
inline uint32_t get_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
inline float get_f32(const uint8_t* p) {
  float f;
  uint32_t u = get_u32(p);
  std::memcpy(&f, &u, 4);
  return f;
}

// ── header ───────────────────────────────────────────────────────────────
struct Header {
  uint32_t length;
  MessageType type;

  // Parse a 16-byte header; returns false on bad magic or type-check.
  static bool parse(const uint8_t* d, Header& out) {
    if (get_u32(d) != kMagic)
      return false;
    out.length = get_u32(d + 4);
    uint32_t t = get_u32(d + 8);
    uint32_t check = get_u32(d + 12);
    if (check != ((~t) & 0xffffffffu))
      return false;
    out.type = static_cast<MessageType>(t);
    return true;
  }
};

inline void append_header(std::vector<uint8_t>& b,
                          MessageType type,
                          uint32_t payloadLen) {
  put_u32(b, kMagic);
  put_u32(b, payloadLen);
  put_u32(b, static_cast<uint32_t>(type));
  put_u32(b, (~static_cast<uint32_t>(type)) & 0xffffffffu);
}

// ── message builders (return full wire frames ready for bulk OUT) ─────────
inline std::vector<uint8_t> frame(MessageType type,
                                  const std::vector<uint8_t>& payload = {}) {
  std::vector<uint8_t> b;
  b.reserve(kHeaderLen + payload.size());
  append_header(b, type, static_cast<uint32_t>(payload.size()));
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

// SendFile: nameLen(u32) + name + '\0', then contentLen(u32) + content.
inline std::vector<uint8_t> send_file(const char* path,
                                      const uint8_t* content,
                                      size_t len) {
  std::vector<uint8_t> p;
  std::string name(path);
  name.push_back('\0');
  put_u32(p, static_cast<uint32_t>(name.size()));
  p.insert(p.end(), name.begin(), name.end());
  put_u32(p, static_cast<uint32_t>(len));
  p.insert(p.end(), content, content + len);
  return frame(MessageType::SendFile, p);
}
inline std::vector<uint8_t> send_number(const char* path, uint32_t value) {
  std::vector<uint8_t> v;
  put_u32(v, value);
  return send_file(path, v.data(), v.size());
}
inline std::vector<uint8_t> send_string(const char* path,
                                        const std::string& s) {
  return send_file(path, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline std::vector<uint8_t> send_command(Command c) {
  std::vector<uint8_t> p;
  put_u32(p, static_cast<uint32_t>(c));
  return frame(MessageType::Command, p);
}
inline std::vector<uint8_t> send_open(const DongleConfig& c) {
  std::vector<uint8_t> p;
  put_u32(p, c.width);
  put_u32(p, c.height);
  put_u32(p, c.fps);
  put_u32(p, c.format);
  put_u32(p, c.packetMax);
  put_u32(p, c.iBoxVersion);
  put_u32(p, c.phoneWorkMode);
  return frame(MessageType::Open, p);
}
// BoxSettings: ASCII JSON payload. syncTime is epoch-ms supplied by caller.
inline std::vector<uint8_t> send_box_settings(const DongleConfig& c,
                                              uint64_t syncTimeMs) {
  std::string json = "{\"mediaDelay\":" + std::to_string(c.mediaDelay) +
                     ",\"syncTime\":" + std::to_string(syncTimeMs) +
                     ",\"androidAutoSizeW\":" + std::to_string(c.width) +
                     ",\"androidAutoSizeH\":" + std::to_string(c.height) + "}";
  std::vector<uint8_t> p(json.begin(), json.end());
  return frame(MessageType::BoxSettings, p);
}
inline std::vector<uint8_t> heartbeat() {
  return frame(MessageType::HeartBeat);
}

// Microphone audio to the dongle: 12-byte audio header (decodeType=5 => 16kHz
// mono, volume=0.0, audioType=3) followed by S16LE PCM.
inline std::vector<uint8_t> send_audio(const int16_t* pcm, size_t samples) {
  std::vector<uint8_t> p;
  put_u32(p, 5);  // decodeType 5: 16000 Hz, mono
  put_u32(p, 0);  // volume 0.0f (bit pattern)
  put_u32(p, 3);  // audioType 3
  const auto* b = reinterpret_cast<const uint8_t*>(pcm);
  p.insert(p.end(), b, b + samples * sizeof(int16_t));
  return frame(MessageType::AudioData, p);
}

// Single-touch. x,y are normalized [0,1]; encoded as fixed-point 0..10000.
inline std::vector<uint8_t> send_touch(float x, float y, TouchAction action) {
  auto clamp01 = [](float v) { return v < 0 ? 0.f : (v > 1 ? 1.f : v); };
  uint32_t fx = static_cast<uint32_t>(clamp01(x) * 10000.f);
  uint32_t fy = static_cast<uint32_t>(clamp01(y) * 10000.f);
  std::vector<uint8_t> p;
  put_u32(p, static_cast<uint32_t>(action));
  put_u32(p, fx);
  put_u32(p, fy);
  put_u32(p, 0);  // flags
  return frame(MessageType::Touch, p);
}

}  // namespace ck
