#pragma once
#include "device_types.h"
namespace scratchsor {
class SensorService;
class StorageService;
uint32_t fingerprintSignature(const DeviceConfig& config);
class CalibrationService {
public:
  enum class Phase : uint8_t { IDLE, BASELINE, COLLECT, DONE, ERROR };
  CalibrationService(SensorService& sensors, StorageService& storage):sensors_(sensors),storage_(storage){}
  void configure(const DeviceConfig& config);
  void begin();
  void update();
  bool startBaseline(uint16_t sampleTarget=100, bool reset=false);
  bool startCollect(float referenceMm, bool test);
  bool finishAndFit();
  bool clearAll();
  bool deletePoint(uint32_t id);
  bool cancel();
  bool busy() const {return phase_==Phase::BASELINE || phase_==Phase::COLLECT;}
  bool hasData() const {return data_.baselineReady || data_.count;}
  bool baselineReady() const {return compatible_ && data_.baselineReady;}
  bool modelValidated() const {return config_.reach.selected && baselineReady() && model().ready;}
  bool selected() const {return config_.reach.selected;}
  uint8_t profile() const {return config_.reach.profile;}
  uint32_t revision() const {return revision_;}
  const ReachConfig& reachConfig() const {return config_.reach;}
  const FingerprintData& dataset() const {return data_;}
  uint16_t pointCount() const {return data_.count;}
  const FingerprintPoint* points() const {return data_.points;}
  const FingerprintModel& model() const {return data_.models[profile()];}
  Phase phase() const {return phase_;}
  const char* phaseName() const;
  const char* detail() const {return detail_;}
  uint16_t collectedThisPoint() const {return collected_;}
  uint16_t collectTarget() const {return target_;}
  void (*onProgress)(const char*,float,uint16_t,uint16_t,const char*)=nullptr;
private:
  bool commit(FingerprintData* next);
  bool fail(const char* detail);
  void notify(const char* detail);
  void start(Phase phase,uint16_t target);
  SensorService& sensors_;
  StorageService& storage_;
  DeviceConfig config_{};
  FingerprintData data_{};
  Phase phase_=Phase::IDLE;
  bool compatible_=true,test_=false;
  uint32_t revision_=0,lastSeq_=0,startedMs_=0;
  uint16_t target_=0,collected_=0;
  float referenceMm_=0;
  double sums_[kFeatureCount]{};
  char detail_[96]="";
};
extern CalibrationService* gpCalib;
}
