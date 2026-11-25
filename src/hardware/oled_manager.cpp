// OledManager — 0.96" 128x64 I2C display on kMuxChOled.
// U8G2's HW_I2C classes drive ::Wire directly; that is safe here because every
// transfer takes I2CBusGuard and re-selects kMuxChOled. After acquisition
// starts, send two tiles per opportunity instead of blocking for a full frame.

#include "hardware/oled_manager.h"
#include "hardware/i2c_manager.h"
#include "diagnostics/diagnostics.h"
#include <U8g2lib.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "product/phone_product.h"
#include "product/app_qr.h"

namespace scratchsor {

OledManager gOled{gI2c};

OledManager::OledManager(I2CManager& bus) : bus_(bus) {}
OledManager::~OledManager() = default;

namespace {
// Auto-rotation ring. Boot/setup screens are not part of it — an unpinned
// screen outside the ring falls back to AP_INFO on the next tick.
OledScreen nextRotating(OledScreen s) {
  switch (s) {
    case OledScreen::AP_INFO:        return OledScreen::STA_STATUS;
    case OledScreen::STA_STATUS:     return OledScreen::SENSOR_HEALTH;
    case OledScreen::SENSOR_HEALTH:  return OledScreen::READINGS;
    case OledScreen::READINGS:       return OledScreen::MEASUREMENT;
    default:                         return OledScreen::AP_INFO;
  }
}
}  // namespace

OledController OledManager::pickController(const OledConfig& cfg) const {
  // I2C cannot distinguish SSD1306 from SH1106; honor the explicit override,
  // default SSD1306 (documented assumption, runtime-overridable).
  if (cfg.controllerOverride == 2) return OledController::SH1106;
  return OledController::SSD1306;
}

bool OledManager::begin(const OledConfig& cfg) {
  cfg_ = cfg;
  controller_ = pickController(cfg);
  addr_ = 0;
  {
    I2CBusGuard guard(bus_, kMuxChOled);
    if (!guard.ok()) {
      controller_ = OledController::UNKNOWN;
      return false;
    }
    if (bus_.probe(kOledAddrPrimary))      addr_ = kOledAddrPrimary;
    else if (bus_.probe(kOledAddrAlt))     addr_ = kOledAddrAlt;
  }
  if (!addr_) {
    controller_ = OledController::UNKNOWN;
    disp_.reset();
    return false;
  }

  U8G2* d =
      (controller_ == OledController::SH1106)
          ? (U8G2*)new U8G2_SH1106_128X64_NONAME_F_HW_I2C(U8G2_R0)
          : (U8G2*)new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0);
  if (!d) {
    controller_ = OledController::UNKNOWN;
    return false;
  }
  disp_.reset(d);

  {
    // begin() itself performs I2C traffic (init sequence) — hold bus+channel.
    I2CBusGuard guard(bus_, kMuxChOled);
    if (!guard.ok()) {
      disp_.reset();
      addr_ = 0;
      controller_ = OledController::UNKNOWN;
      return false;
    }
    disp_->setI2CAddress((uint8_t)(addr_ << 1));  // U8G2 wants the 8-bit addr
    // U8G2 sets Wire's clock on each transfer; keep the shared sensor bus
    // at the configured speed after display traffic as well.
    disp_->setBusClock(kI2cClockHz);
    disp_->begin();
    disp_->setFontPosTop();
    disp_->setFlipMode(cfg_.flipDisplay ? 1 : 0);
    disp_->clearDisplay();
  }

  lastRenderMs_ = 0;
  lastRotateMs_ = millis();
  redrawRequested_ = true;
  refreshPending_ = false;
  nextTile_ = 0;
  return true;
}

void OledManager::update(bool highRateStreaming) {
  if (!present()) return;
  if (refreshPending_) {
    flushChunk();
    return;
  }
  const uint32_t now = millis();
  uint32_t interval =
      1000u / (uint32_t)(cfg_.refreshHz ? cfg_.refreshHz : 1);
  if (highRateStreaming && interval < 1000u) interval = 1000u;
  if (!redrawRequested_ && now - lastRenderMs_ < interval) return;
  if (!pinned_ && cfg_.rotateSec > 0 &&
      now - lastRotateMs_ >= (uint32_t)cfg_.rotateSec * 1000u) {
    lastRotateMs_ = now;
    screen_ = nextRotating(screen_);
  }
  if (render()) {
    redrawRequested_ = false;
    lastRenderMs_ = now;
  }
  if (refreshPending_) flushChunk();
}

void OledManager::show(OledScreen s) {
  screen_ = s;
  pinned_ = true;
  redrawRequested_ = true;
  if (present() && !incremental_ && render()) {
    redrawRequested_ = false;
    lastRenderMs_ = millis();
    lastRotateMs_ = lastRenderMs_;
  }
}

bool OledManager::reconfigure(const OledConfig& cfg) {
  disp_.reset();
  addr_ = 0;
  controller_ = OledController::UNKNOWN;
  return begin(cfg);
}

// ---------------------------------------------------------------------------
// Rendering only changes the RAM framebuffer. Bus access is confined to
// the boot send or short runtime flush, and a pending frame stays immutable.

bool OledManager::render() {
  if (!disp_) return false;
  switch (screen_) {
    case OledScreen::BOOT:          renderBoot(); break;
    case OledScreen::AP_INFO:       renderApInfo(); break;
    case OledScreen::AP_SETUP:      renderApSetup(); break;
    case OledScreen::STA_STATUS:    renderStaStatus(); break;
    case OledScreen::SENSOR_HEALTH: renderSensorHealth(); break;
    case OledScreen::READINGS:      renderReadings(); break;
    case OledScreen::MEASUREMENT:   renderMeasurement(); break;
    case OledScreen::LIVE_SUMMARY:  renderLiveSummary(); break;
  }
  if (incremental_) {
    nextTile_ = 0;
    refreshPending_ = true;
    return true;
  }
  I2CBusGuard guard(bus_, kMuxChOled);
  if (!guard.ok()) return false;
  disp_->sendBuffer();
  lastDisplayedMs_ = millis();
  return true;
}

void OledManager::flushChunk() {
  if (!disp_ || !refreshPending_ || millis() - lastChunkMs_ < 3) return;
  // Zero lock timeout: leave acquisition alone if it already owns the bus.
  I2CBusGuard guard(bus_, kMuxChOled, 0);
  if (!guard.ok()) return;
  constexpr uint8_t columns = kOledWidth / 8;
  constexpr uint8_t tiles = columns * (kOledHeight / 8);
  disp_->updateDisplayArea(nextTile_ % columns, nextTile_ / columns, 2, 1);
  nextTile_ += 2;
  lastChunkMs_ = millis();
  if (nextTile_ >= tiles) {
    refreshPending_ = false;
    lastDisplayedMs_ = lastChunkMs_;
    lastRenderMs_ = lastChunkMs_;
  }
}

namespace {
// Shared chrome: small header + divider, content area y=16..62.
inline void drawHeader(U8G2* d, const char* title) {
  d->setFont(u8g2_font_6x10_tf);
  d->drawStr(0, 1, title);
  d->drawHLine(0, 13, kOledWidth);
}
}  // namespace

void OledManager::renderBoot() {
  disp_->clearBuffer();
  disp_->setFont(u8g2_font_10x20_tf);
  const char* title = kProductName;
  int w = disp_->getStrWidth(title);
  disp_->drawStr((kOledWidth - w) / 2, 16, title);
  disp_->setFont(u8g2_font_6x10_tf);
  const char* msg = "Initializing...";
  w = disp_->getStrWidth(msg);
  disp_->drawStr((kOledWidth - w) / 2, 46, msg);
}

void OledManager::renderApInfo() {
  char buf[40];
  disp_->clearBuffer();
  drawHeader(disp_.get(), "AP Mode");
  snprintf(buf, sizeof(buf), "SSID: %.24s", wifi_.apSsid.c_str());
  disp_->drawStr(0, 17, buf);
  const bool haveIp = wifi_.apIp[0] || wifi_.apIp[1] || wifi_.apIp[2] ||
                      wifi_.apIp[3];
  const String ip = haveIp ? wifi_.apIp.toString() : String("192.168.4.1");
  snprintf(buf, sizeof(buf), "IP: %s", ip.c_str());
  disp_->drawStr(0, 31, buf);
  snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)wifi_.apClients);
  disp_->drawStr(0, 45, buf);
}

void OledManager::renderApSetup() {
  // One-time screen shown right after boot: reveals the generated AP password
  // so the user can join the portal. Deliberately local-only information.
  disp_->clearBuffer();
  // Static layout: 58px QR at left; credentials, URL and status at right.
  disp_->setDrawColor(1);disp_->drawBox(0,3,58,58);disp_->setDrawColor(0);
  for(unsigned y=0;y<29;y++)for(unsigned x=0;x<29;x++)if(appQr[y*4+x/8]&(1u<<(x%8)))disp_->drawBox(x*2,3+y*2,2,2);
  disp_->setDrawColor(1);disp_->setFont(u8g2_font_4x6_tf);
  disp_->drawStr(62,0,"Wi-Fi");disp_->drawStr(62,7,wifi_.apSsid.c_str());
  disp_->drawStr(62,18,"Password");disp_->drawStr(62,25,apPassword_.c_str());
  disp_->drawStr(62,36,"http://");disp_->drawStr(62,43,"192.168.4.1");
  disp_->drawStr(62,55,wifi_.apClients?"Phone joined":"Join Wi-Fi");
}

void OledManager::renderStaStatus() {
  char buf[40];
  disp_->clearBuffer();
  snprintf(buf, sizeof(buf), "Wi-Fi: %s", toString(wifi_.staState));
  drawHeader(disp_.get(), buf);
  if (wifi_.staState == WifiStaState::CONNECTED) {
    snprintf(buf, sizeof(buf), "%.24s", wifi_.staSsid.c_str());
    disp_->drawStr(0, 17, buf);
    snprintf(buf, sizeof(buf), "IP: %s", wifi_.staIp.toString().c_str());
    disp_->drawStr(0, 31, buf);
    snprintf(buf, sizeof(buf), "RSSI: %d dBm", (int)wifi_.staRssi);
    disp_->drawStr(0, 45, buf);
  } else {
    if (wifi_.staSsid.length()) {
      snprintf(buf, sizeof(buf), "%.24s", wifi_.staSsid.c_str());
      disp_->drawStr(0, 17, buf);
    } else {
      disp_->drawStr(0, 17, "AP only (no STA)");
    }
    if (wifi_.staError.length()) {
      snprintf(buf, sizeof(buf), "Err: %.24s", wifi_.staError.c_str());
      disp_->drawStr(0, 31, buf);
    }
    if (wifi_.apClients || wifi_.apSsid.length()) {
      snprintf(buf, sizeof(buf), "AP: %u client(s)", (unsigned)wifi_.apClients);
      disp_->drawStr(0, 45, buf);
    }
  }
}

void OledManager::renderSensorHealth() {
  char buf[40];
  disp_->clearBuffer();
  drawHeader(disp_.get(), "Sensors");
  for (uint8_t i = 0; i < kSensorCount; ++i) {
    snprintf(buf, sizeof(buf), "S%u:%s", (unsigned)(i + 1),
             toString(health_.sensor[i].status));
    disp_->drawStr((i % 2) ? 66 : 0, (i < 2) ? 17 : 31, buf);
  }
  snprintf(buf, sizeof(buf), "Rate: %.1f Hz", (double)health_.measuredRateHz);
  disp_->drawStr(0, 45, buf);
}

void OledManager::renderReadings() {
  char buf[40];
  const SensorReading& r = frame_.sensors[readingSensor_];
  disp_->clearBuffer();
  snprintf(buf, sizeof(buf), "Sensor: %u", (unsigned)readingSensor_ + 1);
  drawHeader(disp_.get(), buf);
  if (r.valid) {
    snprintf(buf, sizeof(buf), "Bx: %.2f uT", (double)r.raw.x);
    disp_->drawStr(0, 17, buf);
    snprintf(buf, sizeof(buf), "By: %.2f uT", (double)r.raw.y);
    disp_->drawStr(0, 30, buf);
    snprintf(buf, sizeof(buf), "Bz: %.2f uT", (double)r.raw.z);
    disp_->drawStr(0, 43, buf);
    snprintf(buf, sizeof(buf), "|B|: %.2f uT", (double)r.magnitudeRaw);
    disp_->drawStr(0, 56, buf);
  } else {
    snprintf(buf, sizeof(buf), "State: %s", toString(r.status));
    disp_->drawStr(0, 17, buf);
    disp_->drawStr(0, 31, "No valid data");
  }
}

void OledManager::renderMeasurement() {
  disp_->clearBuffer();disp_->setFont(u8g2_font_5x8_tf);
  String name=gPhone.patient().length()?gPhone.patient():"-";
  bool thai=false;for(unsigned i=0;i<name.length();i++)if(uint8_t(name[i])>=0xE0)thai=true;
  if(thai)disp_->setFont(u8g2_font_etl14thai_t);
  while(name.length()&&disp_->getUTF8Width(name.c_str())>100){unsigned end=name.length()-1;while(end&&((uint8_t(name[end])&0xC0)==0x80))--end;name=name.substring(0,end);}
  disp_->drawUTF8(0,0,name.c_str());disp_->setFont(u8g2_font_5x8_tf);
  const String demographics=(gPhone.gender()=="male"?String("M"):gPhone.gender()=="female"?String("F"):String("-"))+"/"+(gPhone.hand()=="right"?"R":gPhone.hand()=="left"?"L":"-");
  disp_->drawStr(108,1,demographics.c_str());
  for(unsigned i=0;i<4;i++){char label[4];snprintf(label,sizeof label,"S%u",i+1);unsigned x=i*32;disp_->drawStr(x,16,label);
    const auto& s=frame_.sensors[i];bool ok=s.valid&&!s.saturated&&s.status==SensorStatus::OK&&millis()-s.timestampMs<150;
    if(ok)disp_->drawDisc(x+20,20,3);else disp_->drawCircle(x+20,20,3);}
  const char* category=gPhone.measuring()?"Measuring":gPhone.category();
  if(!strcmp(category,"Above standard")||!strcmp(category,"Above range"))category="HIGHER";else if(!strcmp(category,"Below standard")||!strcmp(category,"Below zero"))category="BELOW";else if(!strcmp(category,"Standard"))category="STANDARD";
  else if(!std::isfinite(gPhone.result())&&!gPhone.measuring())category=!strcmp(gPhone.category(),"Unsure")?"UNSURE":"READY";
  disp_->setFont(u8g2_font_7x13_tf);int width=disp_->getStrWidth(category);disp_->drawStr((128-width)/2,29,category);
  char value[24];if(gPhone.belowZero())snprintf(value,sizeof value,"<0 cm (est.)");else if(gPhone.aboveEight())snprintf(value,sizeof value,">8 cm (est.)");else if(std::isfinite(gPhone.result()))snprintf(value,sizeof value,"%.2f cm",double(gPhone.result()));else snprintf(value,sizeof value,"-- cm");
  disp_->setFont(u8g2_font_6x10_tf);width=disp_->getStrWidth(value);disp_->drawStr((128-width)/2,44,value);
  disp_->setFont(u8g2_font_5x8_tf);disp_->drawStr(0,57,gPhone.saved()?"Saved on phone":gPhone.measuring()?"Hold still for 1 sec":gPhone.connected()?"Phone connected":"Phone disconnected");
}

void OledManager::renderLiveSummary() {
  disp_->clearBuffer();
  drawHeader(disp_.get(), "Raw magnetic data");
  unsigned onlineCount = 0;
  for (const auto& r : frame_.sensors) {
    if (r.valid && r.status == SensorStatus::OK && !r.saturated && millis()-r.timestampMs<=1000) ++onlineCount;
  }
  char buf[32];
  disp_->setFont(u8g2_font_7x13_tf);
  snprintf(buf, sizeof(buf), "%u/%u sensors online", onlineCount, unsigned(kSensorCount));
  disp_->drawStr(0,18,buf);
  disp_->setFont(u8g2_font_5x8_tf);
  snprintf(buf, sizeof(buf), "Rate: %.1f Hz", double(health_.measuredRateHz));
  disp_->drawStr(0,34,buf);
  disp_->drawStr(0,44,"Live graph on PC");
  for(unsigned i=0;i<kSensorCount;++i) {
    char label[4];snprintf(label,sizeof label,"S%u",i+1);
    const unsigned x=i*32;disp_->drawStr(x,54,label);
    const auto& r=frame_.sensors[i];
    const bool online=r.valid && r.status==SensorStatus::OK && !r.saturated && millis()-r.timestampMs<=1000;
    // SSD1306/SH1106 are monochrome: filled=online, hollow=offline.
    if(online)disp_->drawDisc(x+20,58,3);else disp_->drawCircle(x+20,58,3);
  }
}
} // namespace scratchsor
