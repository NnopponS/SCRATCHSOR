#pragma once
// SCRATCHSOR global configuration constants.
// Board-specific pins live in pins.h. Everything here is a compile-time
// default; user-adjustable settings are stored in NVS (see storage_service).

#include <cstdint>

namespace scratchsor {

// ---- Identity ----
inline constexpr const char* kProductName   = "SCRATCHSOR";
inline constexpr const char* kFirmwareVer   = "0.3.0-range-v5";
inline constexpr const char* kApiVersion    = "v1";
inline constexpr uint32_t    kConfigSchema  = 3;

// ---- I2C primary bus ----
inline constexpr uint32_t kI2cClockHz       = 100000; // bench sensor ACKs reliably at 100 kHz
// A full 128x64 OLED buffer needs >92 ms at 100 kHz. Allow that guarded
// transfer to finish before classifying an acquisition wait as a bus fault.
inline constexpr uint32_t kI2cLockTimeoutMs = 150;

// ---- PCA9548A / TCA9548A mux ----
inline constexpr uint8_t  kMuxAddrBase      = 0x70;  // A2..A0 all low
inline constexpr uint8_t  kMuxAddrMax       = 0x77;
inline constexpr uint8_t  kMuxChannels      = 8;
// Channel allocation (see docs/wiring.md).
// OLED on SD0/SC0; sensor IDs 0..3 on SD2/SC2 through SD5/SC5.
inline constexpr uint8_t  kMuxChSensor[4]   = {2, 3, 4, 5};
inline constexpr uint8_t  kMuxChOled        = 0;

// ---- MLX90393 ----
// CJMCU-90393 modules usually strap to 0x0C; driver scans 0x0C..0x0F on each
// sensor channel at init and remembers the address it found.
inline constexpr uint8_t  kMlxAddrMin       = 0x0C;
inline constexpr uint8_t  kMlxAddrMax       = 0x0F;
inline constexpr uint8_t  kSensorCount      = 4;

// ---- OLED ----
inline constexpr uint8_t  kOledAddrPrimary  = 0x3C;
inline constexpr uint8_t  kOledAddrAlt      = 0x3D;
inline constexpr uint8_t  kOledWidth        = 128;
inline constexpr uint8_t  kOledHeight       = 64;
inline constexpr uint8_t  kOledRefreshHz    = 4;     // limit display churn
inline constexpr uint8_t  kOledRotateSec    = 5;     // auto screen rotation

// ---- Acquisition ----
inline constexpr float    kSampleRateHz         = 50.0f; // complete 4-sensor frames
inline constexpr float    kSampleRateMinHz      = 1.0f;
inline constexpr float    kSampleRateMaxHz      = 50.0f;
inline constexpr uint8_t  kAcquisitionCore      = 0;     // keep loop()/WiFi on core 1
inline constexpr uint16_t kAcquisitionStack     = 8192;
inline constexpr uint8_t  kAcquisitionPrio      = 2;

// ---- Networking ----
inline constexpr uint8_t   kApChannel        = 1;
inline constexpr uint8_t   kApMaxClients     = 4;
inline constexpr uint32_t  kStaConnectTimeoutMs = 15000;
inline constexpr uint32_t  kStaRetryBackoffMs   = 30000;
inline constexpr uint16_t  kHttpPort          = 80;
inline constexpr uint16_t  kWsPort            = 81;
inline constexpr uint16_t  kDnsPort           = 53;
inline constexpr uint8_t   kApPasswordLen     = 10;

// ---- WebSocket broadcast ----
inline constexpr uint32_t  kWsSensorIntervalMs = 20;    // up to 50 Hz live frames
inline constexpr uint32_t  kWsUiSensorIntervalMs = 50;  // phone preview; raw capture remains 50 Hz
inline constexpr uint32_t  kSerialBaud         = 921600;
inline constexpr uint16_t  kWsJsonBufBytes     = 2048;

// ---- Recording / buffers ----
inline constexpr uint16_t  kRecordMaxFrames    = 256;
inline constexpr uint32_t  kRecordDurationMs   = 3000;
static_assert(kRecordMaxFrames >= kRecordDurationMs * kSampleRateMaxHz / 1000,
              "Recorder must retain a full trial at the maximum sample rate");
inline constexpr uint16_t  kCalibMaxPoints     = 64;    // reference distances
inline constexpr uint16_t  kCalibMaxSamples    = 128;   // samples per point

// ---- NVS ----
inline constexpr const char* kNvsNamespace    = "scratchsor";
inline constexpr const char* kNvsKeyConfig    = "cfg";
inline constexpr const char* kNvsKeyWifiSsid  = "w_ssid";
inline constexpr const char* kNvsKeyWifiPass  = "w_pass";
inline constexpr const char* kNvsKeyApPass    = "ap_pass";
inline constexpr const char* kNvsKeyCalib     = "calib";

}  // namespace scratchsor
