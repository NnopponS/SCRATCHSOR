// SCRATCHSOR — NVS persistence (Preferences).
// Flash-wear discipline: callers decide when to save; nothing here is on a
// hot path. Corrupt/absent data falls back to defaults — never brick on NVS.

#include "services/storage_service.h"
#include "diagnostics/diagnostics.h"
#include <cstdlib>
#include <cstring>
#include <new>
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_random.h>
#endif

namespace scratchsor {

StorageService gStorage;

namespace {

constexpr uint32_t kCalibMagic = 0x53435243UL;   // 'SCRC'
// AP password charset: 32 unambiguous chars (no 0/O/1/I/L).
constexpr char kApCharset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

#pragma pack(push, 1)
struct CalibHeader {
  uint32_t magic;
  uint16_t nPoints;
  uint16_t crc;      // CRC-16/CCITT-FALSE over the payload (points + model)
};
#pragma pack(pop)

// Charset length is 32 == 2^5, so masking gives a uniform index.
uint8_t apRandomIndex() {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
  return (uint8_t)(esp_random() & 31);
#else
  return (uint8_t)(std::rand() & 31);   // native-test fallback
#endif
}

}  // namespace

bool StorageService::begin() {
  const bool configReady=prefs_.begin(kNvsNamespace, false);
  fingerprintStorageReady_=fingerprintPrefs_.begin("reach",false,"reach_nvs");
  if(!fingerprintStorageReady_)SCR_LOGE("Fingerprint NVS unavailable: upload firmware with partitions.csv");
  return configReady && fingerprintStorageReady_;
}

// ----------------------------- config ----------------------------------

bool StorageService::loadConfig(DeviceConfig& out) {
  const size_t len = prefs_.getBytesLength(kNvsKeyConfig);
  struct LegacyConfig {
    uint32_t schema; char deviceName[32]; bool apPersistent;
    SensorConfig sensor; FilterConfig filter; OledConfig oled;
  };
  struct Version2Config {
    uint32_t schema; char deviceName[32]; bool apPersistent;
    SensorConfig sensor; FilterConfig filter; OledConfig oled; GapConfig gap;
  };
  if (len == sizeof(Version2Config)) {
    Version2Config old{};
    if (prefs_.getBytes(kNvsKeyConfig, &old, sizeof old) != sizeof old || old.schema != 2) return false;
    out = DeviceConfig{};
    memcpy(out.deviceName, old.deviceName, sizeof out.deviceName);
    out.deviceName[sizeof out.deviceName-1]=0;
    out.apPersistent=old.apPersistent;out.sensor=old.sensor;out.filter=old.filter;
    out.oled=old.oled;out.gap=old.gap;
    // Fingerprinting always uses the saved ambient-field baseline.
    out.filter.baselineRemoval=true;
    return true;
  }
  if (len == sizeof(LegacyConfig)) {
    LegacyConfig old{};
    if (prefs_.getBytes(kNvsKeyConfig, &old, sizeof old) != sizeof old || old.schema != 1)
      return false;
    out = DeviceConfig{};
    memcpy(out.deviceName, old.deviceName, sizeof out.deviceName);
    out.deviceName[sizeof out.deviceName - 1] = 0;
    out.apPersistent = old.apPersistent;
    out.sensor = old.sensor; out.filter = old.filter; out.oled = old.oled;
    out.filter.baselineRemoval=true;
    return true;
  }
  if (len != sizeof(DeviceConfig)) return false;
  DeviceConfig tmp{};
  if (prefs_.getBytes(kNvsKeyConfig, &tmp, sizeof tmp) != sizeof tmp) {
    return false;
  }
  if (tmp.schema != kConfigSchema) return false;   // schema gate, no migration
  if(tmp.reach.profile>=4 || !memchr(tmp.reach.setup,0,sizeof tmp.reach.setup))return false;
  out = tmp;
  return true;
}

bool StorageService::saveConfig(const DeviceConfig& cfg) {
  return prefs_.putBytes(kNvsKeyConfig, &cfg, sizeof cfg) == sizeof cfg;
}

// ------------------------------ Wi-Fi ----------------------------------

bool StorageService::loadWifi(String& ssid, String& pass) {
  ssid = prefs_.getString(kNvsKeyWifiSsid, "");
  pass = prefs_.getString(kNvsKeyWifiPass, "");
  return ssid.length() > 0;
}

bool StorageService::saveWifi(const String& ssid, const String& pass) {
  const size_t a = prefs_.putString(kNvsKeyWifiSsid, ssid);
  const size_t b = prefs_.putString(kNvsKeyWifiPass, pass);
  return a == ssid.length() && b == pass.length();
}

bool StorageService::clearWifi() {
  const bool a = prefs_.remove(kNvsKeyWifiSsid);
  const bool b = prefs_.remove(kNvsKeyWifiPass);
  return a || b;   // false only when neither key existed
}

String StorageService::loadOrCreateApPassword() {
  String pw = prefs_.getString(kNvsKeyApPass, "");
  if (pw.length() == kApPasswordLen) return pw;
  String gen;
  if (regenerateApPassword(gen)) return gen;
  // NVS write failed — the generated password is still usable for this boot.
  if (gen.length() == kApPasswordLen) {
    SCR_LOGW("AP password generated but could not be persisted");
    return gen;
  }
  SCR_LOGE("AP password unavailable");
  return String();
}

bool StorageService::regenerateApPassword(String& out) {
  char buf[kApPasswordLen + 1];
  for (uint8_t i = 0; i < kApPasswordLen; ++i) {
    buf[i] = kApCharset[apRandomIndex()];
  }
  buf[kApPasswordLen] = 0;
  out = buf;
  return prefs_.putString(kNvsKeyApPass, out) == kApPasswordLen;
}

// --------------------------- calibration -------------------------------

bool StorageService::saveCalibration(const CalibrationPoint* pts, uint16_t n,
                                     const CalibrationModel& model) {
  if (n > kCalibMaxPoints || (n && pts == nullptr)) return false;

  // Refresh the embedded per-model CRC (field zeroed while hashing).
  CalibrationModel m = model;
  {
    CalibrationModel z = m;
    z.crc = 0;
    m.crc = crc16((const uint8_t*)&z, sizeof z);
  }

  const size_t payload = sizeof(CalibrationPoint) * n + sizeof m;
  const size_t total = sizeof(CalibHeader) + payload;
  uint8_t* buf = (uint8_t*)malloc(total);
  if (!buf) return false;

  CalibHeader h;
  h.magic = kCalibMagic;
  h.nPoints = n;
  h.crc = 0;
  if (n) memcpy(buf + sizeof h, pts, sizeof(CalibrationPoint) * n);
  memcpy(buf + sizeof h + sizeof(CalibrationPoint) * n, &m, sizeof m);
  h.crc = crc16(buf + sizeof h, payload);
  memcpy(buf, &h, sizeof h);

  const size_t w = prefs_.putBytes(kNvsKeyCalib, buf, total);
  free(buf);
  return w == total;
}

bool StorageService::loadCalibration(CalibrationPoint* pts, uint16_t maxPts,
                                     uint16_t& nOut, CalibrationModel& model) {
  nOut = 0;
  const size_t len = prefs_.getBytesLength(kNvsKeyCalib);
  if (len < sizeof(CalibHeader) + sizeof(CalibrationModel)) return false;

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) return false;
  bool ok = false;
  if (prefs_.getBytes(kNvsKeyCalib, buf, len) == len) {
    CalibHeader h;
    memcpy(&h, buf, sizeof h);
    const size_t expect = sizeof(CalibHeader) +
        (size_t)h.nPoints * sizeof(CalibrationPoint) + sizeof(CalibrationModel);
    if (h.magic == kCalibMagic && expect == len &&
        h.nPoints <= kCalibMaxPoints && h.nPoints <= maxPts && pts) {
      if (crc16(buf + sizeof h, len - sizeof h) == h.crc) {
        if (h.nPoints) {
          memcpy(pts, buf + sizeof h,
                 (size_t)h.nPoints * sizeof(CalibrationPoint));
        }
        memcpy(&model,
               buf + sizeof h + (size_t)h.nPoints * sizeof(CalibrationPoint),
               sizeof model);
        // Per-model integrity: drop the model if its own CRC doesn't verify
        // (the blob CRC covers everything; this is defence in depth).
        CalibrationModel z = model;
        const uint16_t saved = z.crc;
        z.crc = 0;
        if (crc16((const uint8_t*)&z, sizeof z) != saved) {
          model = CalibrationModel{};
        }
        nOut = h.nPoints;
        ok = true;
      } else {
        SCR_LOGW("calibration blob CRC mismatch — dataset ignored");
      }
    }
  }
  free(buf);
  return ok;
}

bool StorageService::clearCalibration() {
  return prefs_.remove(kNvsKeyCalib);   // dataset+model only
}

namespace {
struct FingerprintBlob {
  uint32_t magic = 0x4B4E4E31;
  uint16_t version = 1, crc = 0;
  FingerprintData data;
};
}
bool StorageService::saveFingerprints(const FingerprintData& data) {
  if(!fingerprintStorageReady_ || !validFingerprintData(data))return false;
  auto* blob = new (std::nothrow) FingerprintBlob{};
  if(!blob)return false;
  blob->data=data;
  blob->crc=crc16(reinterpret_cast<const uint8_t*>(&blob->data),sizeof blob->data);
  const bool ok=fingerprintPrefs_.putBytes("fingerprint",blob,sizeof *blob)==sizeof *blob;
  delete blob;return ok;
}
bool StorageService::loadFingerprints(FingerprintData& data) {
  if(!fingerprintStorageReady_ || fingerprintPrefs_.getBytesLength("fingerprint")!=sizeof(FingerprintBlob))return false;
  auto* blob=new (std::nothrow) FingerprintBlob{};
  if(!blob)return false;
  const bool ok=fingerprintPrefs_.getBytes("fingerprint",blob,sizeof *blob)==sizeof *blob &&
    blob->magic==0x4B4E4E31 && blob->version==1 &&
    blob->crc==crc16(reinterpret_cast<const uint8_t*>(&blob->data),sizeof blob->data) &&
    validFingerprintData(blob->data);
  if(ok)data=blob->data;
  delete blob;return ok;
}

// --------------------------- factory reset -----------------------------

bool StorageService::factoryResetKeepWifi() {
  const bool a = prefs_.remove(kNvsKeyConfig);
  const bool b = prefs_.remove(kNvsKeyCalib);
  const bool c = fingerprintPrefs_.remove("fingerprint");
  return a || b || c;   // Wi-Fi + AP password keys untouched
}

bool StorageService::factoryResetAll() {
  const bool fingerprints=fingerprintPrefs_.clear();
  return prefs_.clear() && fingerprints;
}

// ------------------------------ crc ------------------------------------

uint16_t StorageService::crc16(const uint8_t* d, size_t n) {
  // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection.
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

}  // namespace scratchsor
