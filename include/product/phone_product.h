#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "device_types.h"
namespace scratchsor {
struct PhoneRawFrame {uint32_t seq=0,ts=0;float axes[12]{};bool healthy=false;uint8_t validMask=0,satMask=0;SensorStatus status[4]{};};
class PhoneProduct {
public:
  void begin();
  void onFrame(const SensorFrame& frame); // acquisition task; bounded RAM copy only
  void update();                         // app loop owns all model/UI state
  bool command(JsonVariantConst body,JsonDocument& response,String& error);
  void status(JsonDocument& doc) const;
  bool connected() const {return owner_.length() && millis()-heartbeat_<15000;}
  bool measuring() const {return active_;}
  const String& patient() const {return name_;}
  const String& gender() const {return gender_;}
  const String& hand() const {return hand_;}
  const char* category() const {return category_;}
  const char* state() const {return state_;}
  float result() const {return result_;}
  bool belowZero() const {return belowZero_;}
  bool aboveEight() const {return aboveEight_;}
  const char* modelVersion() const;
  bool pendingCapture() const {return id_.length()&&!saved_;}
  bool saved() const {return saved_;}
private:
  static constexpr unsigned kQueue=96,kRaw=64;
  mutable portMUX_TYPE mutex_=portMUX_INITIALIZER_UNLOCKED;
  PhoneRawFrame queue_[kQueue],raw_[kRaw],window_[16];
  unsigned read_=0,write_=0,queued_=0,rawCount_=0,windowCount_=0;
  bool overflow_=false,magnet_=false,saved_=false;volatile bool active_=false,failed_=false;
  uint32_t started_=0,lastFrame_=0,lastSeq_=0,lastPrediction_=0,heartbeat_=0;
  float latestFeatures_[16]{},captureFeatures_[16][16]{};unsigned featureCount_=0;
  float captureValues_[16]{};uint32_t featureFirst_=0,featureLast_=0;bool belowZero_=false,aboveEight_=false;float rangeConfidence_=0;
  String resultReason_;const char* liveReason_="Waiting for sensor readings";
  String owner_,name_,gender_,hand_,patientId_,id_;float result_=NAN;
  float background_[12]{},off_=20,on_=30,rules_[4][2]{{4,6},{0,3},{5,7},{0,6}};
  bool calibrating_=false,calibrationFailed_=false;uint32_t calibrationStarted_=0,calibrationFirst_=0,calibrationLast_=0;unsigned calibrationCount_=0,calibrationInvalid_=0;
  float calibration_[256][12]{};String calibrationMessage_;const char* calibrationStatus_="IDLE";bool customReference_=false;
  void finishCalibration();
  const char* state_="WARMING_UP";const char* category_="—";const char* outcome_="";
  void process(const PhoneRawFrame& frame);
  void finish();
};
extern PhoneProduct gPhone;
}
