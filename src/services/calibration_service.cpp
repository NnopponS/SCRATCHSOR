#include "services/calibration_service.h"
#include "services/sensor_service.h"
#include "services/storage_service.h"
#include <cstring>
#include <new>
namespace scratchsor {
CalibrationService* gpCalib=nullptr;
uint32_t fingerprintSignature(const DeviceConfig& c) {
  uint32_t hash=2166136261u;
  auto add=[&hash](const auto& value){const auto* p=reinterpret_cast<const uint8_t*>(&value);
    for(unsigned i=0;i<sizeof value;++i) hash=(hash^p[i])*16777619u;};
  // Hash fields explicitly: persisted struct padding is not a setup identifier.
  for(char x:c.reach.setup) {add(x);if(!x)break;}
  for(auto channel:kMuxChSensor)add(channel);
  add(c.sensor.gain);add(c.sensor.resolution);add(c.sensor.oversampling);
  add(c.sensor.digitalFilter);add(c.sensor.sampleRateHz);add(c.sensor.readTemperature);
  add(c.filter.movingAvgEnabled);add(c.filter.movingAvgWindow);add(c.filter.medianEnabled);
  add(c.filter.medianWindow);add(c.filter.lowpassEnabled);add(c.filter.lowpassAlpha);
  add(c.filter.baselineRemoval);add(c.filter.outlierSigma);
  add(kFeatureFrames);add(kFeatureCount);
  return hash;
}
void CalibrationService::configure(const DeviceConfig& c) {
  config_=c;compatible_=!hasData() || data_.signature==fingerprintSignature(c);++revision_;
}
void CalibrationService::begin() {
  storage_.loadFingerprints(data_);
  compatible_=!hasData() || data_.signature==fingerprintSignature(config_);
  if(baselineReady()) {
    Vec3 baseline[kSensorCount];
    for(unsigned i=0;i<kSensorCount;++i)baseline[i]={data_.baseline[i*3],data_.baseline[i*3+1],data_.baseline[i*3+2]};
    sensors_.setBaselines(baseline);
    // Recompute persisted scaling/scores; never trust a saved ready flag alone.
    for(uint8_t p=0;p<4;++p)if(data_.models[p].ready)fitFingerprint(data_,p);
  }
  notify(compatible_?"Ready for calibration":"Setup changed: reset calibration required");
}
const char* CalibrationService::phaseName() const {
  switch(phase_) {case Phase::BASELINE:return "baseline";case Phase::COLLECT:return "collect";
    case Phase::DONE:return "done";case Phase::ERROR:return "error";default:return "idle";}
}
void CalibrationService::notify(const char* text) {
  if(text!=detail_)strlcpy(detail_,text,sizeof detail_);
  if(onProgress)onProgress(phaseName(),referenceMm_,collected_,target_,detail_);
}
bool CalibrationService::fail(const char* text) {notify(text);return false;}
bool CalibrationService::commit(FingerprintData* next) {
  if(!next)return fail("Out of memory; previous data retained");
  const bool ok=storage_.saveFingerprints(*next);
  if(ok){data_=*next;compatible_=!hasData() || data_.signature==fingerprintSignature(config_);++revision_;}
  delete next;
  return ok || fail("Storage write failed; previous data retained");
}
void CalibrationService::start(Phase phase,uint16_t target) {
  phase_=phase;target_=target;collected_=0;startedMs_=millis();
  std::memset(sums_,0,sizeof sums_);
  SensorFrame frame;lastSeq_=sensors_.latest(frame)?frame.seq:0;++revision_;
}
bool CalibrationService::clearAll() {
  if(busy())return fail("Cancel capture before resetting");
  auto* next=new(std::nothrow) FingerprintData{};
  if(next)next->signature=fingerprintSignature(config_);
  if(!commit(next))return false;
  sensors_.clearBaselines();phase_=Phase::IDLE;collected_=target_=0;
  notify("All fingerprint profiles reset; legacy distance data kept");return true;
}
bool CalibrationService::startBaseline(uint16_t samples,bool reset) {
  if(busy())return fail("Capture already running");
  if(!selected() || !config_.reach.setup[0])return fail("Select profile and enter setup identifier first");
  if(hasData() && !reset)return fail("Explicit reset required before replacing baseline");
  if(!clearAll())return false;
  sensors_.clearBaselines();referenceMm_=0;
  start(Phase::BASELINE,samples<20?20:samples>128?128:samples);
  notify("Remove magnets; keep sensor mounting unchanged");return true;
}
bool CalibrationService::startCollect(float mm,bool test) {
  if(busy())return fail("Capture already running");
  if(!selected() || !baselineReady())return fail("Select profile and capture a compatible baseline first");
  if(!referenceAllowed(mm))return fail("Reference must be -50 to +50 cm, including zero");
  if(data_.count>=kFingerprintCapacity)return fail("Dataset full; delete a trial first");
  if(data_.nextId==UINT32_MAX)return fail("Trial identifiers exhausted; reset required");
  referenceMm_=mm;test_=test;start(Phase::COLLECT,kFeatureFrames);
  notify(test?"Separate test trial; reposition before capture":"Training trial; reposition before capture");return true;
}
bool CalibrationService::cancel() {
  if(!busy())return fail("No capture running");
  phase_=Phase::IDLE;++revision_;notify("Capture cancelled; previous trials retained");return true;
}
void CalibrationService::update() {
  if(!busy())return;
  if(millis()-startedMs_>30000) {phase_=Phase::ERROR;++revision_;notify("Capture timed out; check all four sensors");return;}
  SensorFrame frame;float x[kFeatureCount];
  if(!sensors_.latest(frame) || !fingerprintFeatures(frame,millis(),x)) {
    collected_=0;std::memset(sums_,0,sizeof sums_);return;
  }
  if(frame.seq==lastSeq_)return;
  if(lastSeq_ && frame.seq!=lastSeq_+1){collected_=0;std::memset(sums_,0,sizeof sums_);}
  lastSeq_=frame.seq;
  for(unsigned j=0;j<kFeatureCount;++j)sums_[j]+=x[j];++collected_;
  if(collected_<target_){notify(detail_);return;}
  auto* next=new(std::nothrow) FingerprintData(data_);
  const bool baseline=phase_==Phase::BASELINE;
  if(next) {
    if(baseline) {
      for(unsigned j=0;j<kFeatureCount;++j)next->baseline[j]=float(sums_[j]/target_);
      next->baselineReady=true;next->signature=fingerprintSignature(config_);
    } else {
      FingerprintPoint point{};point.id=next->nextId++;point.timestampMs=millis();
      point.profile=profile();point.test=test_;point.referenceMm=referenceMm_;
      for(unsigned j=0;j<kFeatureCount;++j)point.features[j]=float(sums_[j]/target_);
      if(!appendFingerprint(*next,point)){delete next;phase_=Phase::ERROR;notify("Trial rejected; data unchanged");return;}
    }
  }
  if(!commit(next)){phase_=Phase::ERROR;return;}
  if(baseline) {
    Vec3 b[kSensorCount];
    for(unsigned i=0;i<kSensorCount;++i)b[i]={data_.baseline[i*3],data_.baseline[i*3+1],data_.baseline[i*3+2]};
    sensors_.setBaselines(b);
  }
  phase_=Phase::IDLE;notify(baseline?"Baseline saved":"Trial saved; reposition before the next trial");
}
bool CalibrationService::finishAndFit() {
  if(busy())return fail("Finish or cancel capture first");
  if(!selected() || !baselineReady())return fail("Compatible baseline required");
  auto* next=new(std::nothrow) FingerprintData(data_);
  if(!next)return fail("Out of memory");
  if(!fitFingerprint(*next,profile())) {delete next;return fail("Need 8 training and 2 separate test trials in EACH category");}
  if(!commit(next))return false;
  phase_=Phase::DONE;notify("Profile ready; validation score is advisory");return true;
}
bool CalibrationService::deletePoint(uint32_t id) {
  if(busy())return fail("Finish or cancel capture first");
  auto* next=new(std::nothrow) FingerprintData(data_);
  if(!next)return fail("Out of memory");
  if(!deleteFingerprint(*next,id)){delete next;return fail("Trial not found");}
  if(!commit(next))return false;
  phase_=Phase::IDLE;notify("Trial deleted; validate its profile again");return true;
}
}
