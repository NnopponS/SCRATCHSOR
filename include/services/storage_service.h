#pragma once
// NVS (Preferences) persistence. Versioned schema + CRC for calibration.
// Flash-wear discipline: callers decide when to save; nothing here is on a
// hot path. Corrupt/absent data falls back to defaults — never brick on NVS.

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "device_types.h"

namespace scratchsor {

class StorageService {
public:
  bool begin();   // open NVS namespace (RW)

  // Device config.
  bool loadConfig(DeviceConfig& out);     // false => defaults kept
  bool saveConfig(const DeviceConfig& cfg);

  // Wi-Fi STA credentials (write-only from API; read at boot only).
  bool loadWifi(String& ssid, String& pass);
  bool saveWifi(const String& ssid, const String& pass);
  bool clearWifi();

  // Per-device AP password: load existing or generate+persist a new one.
  String loadOrCreateApPassword();
  bool regenerateApPassword(String& out);  // new random, persisted

  // Calibration dataset + validated model blob.
  bool saveCalibration(const CalibrationPoint* pts, uint16_t n,
                       const CalibrationModel& model);
  bool loadCalibration(CalibrationPoint* pts, uint16_t maxPts, uint16_t& nOut,
                       CalibrationModel& model);
  bool clearCalibration();                 // wipes dataset+model ONLY
  bool saveFingerprints(const FingerprintData& data);
  bool loadFingerprints(FingerprintData& data);

  bool factoryResetKeepWifi();             // cfg + calib cleared, wifi kept
  bool factoryResetAll();

private:
  Preferences prefs_;
  Preferences fingerprintPrefs_;
  bool fingerprintStorageReady_ = false;
  uint16_t crc16(const uint8_t* d, size_t n);
};

extern StorageService gStorage;

}  // namespace scratchsor
