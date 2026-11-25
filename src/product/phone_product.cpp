#include "product/phone_product.h"
#include "product/model_v4.h"
#include "product/range_v5.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <Preferences.h>
namespace scratchsor {
PhoneProduct gPhone;
const char* PhoneProduct::modelVersion() const {return range_model::version;}
namespace {float med(float* a,unsigned n){std::sort(a,a+n);return n%2?a[n/2]:(a[n/2-1]+a[n/2])/2;}}
void PhoneProduct::begin(){
  memcpy(background_,phone_model::background,sizeof background_);off_=phone_model::magnetOff;on_=phone_model::magnetOn;
  struct Reference{char model[32];float axes[12],off,on;};Reference ref{};
  Preferences p;if(p.begin("phone_ref",true)){if(p.getBytesLength("ref")==sizeof ref){p.getBytes("ref",&ref,sizeof ref);bool valid=!strncmp(ref.model,phone_model::version,sizeof ref.model)&&std::isfinite(ref.off)&&std::isfinite(ref.on)&&ref.off>0&&ref.on>ref.off;for(float x:ref.axes)valid&=std::isfinite(x);if(valid){memcpy(background_,ref.axes,sizeof background_);off_=ref.off;on_=ref.on;customReference_=true;}}p.end();}
  if(p.begin("phone_rules",true)){float r[4][2];if(p.getBytesLength("rules")==sizeof r){p.getBytes("rules",r,sizeof r);bool valid=true;for(auto& row:r)valid&=std::isfinite(row[0])&&std::isfinite(row[1])&&row[0]<row[1]&&row[0]>=-50&&row[1]<=50;if(valid)memcpy(rules_,r,sizeof r);}p.end();}
}
void PhoneProduct::finishCalibration(){
  calibrating_=false;calibrationStatus_="FAILED";
  if(calibrationFailed_||calibrationCount_<200||calibrationInvalid_*10>calibrationCount_+calibrationInvalid_||millis()-lastFrame_>150||uint32_t(calibrationLast_-calibrationFirst_)<4500){calibrationMessage_="Capture failed: keep all four sensors connected. Previous reference retained.";return;}
  float center[12],values[256],scores[256];
  for(unsigned a=0;a<12;a++){for(unsigned i=0;i<calibrationCount_;i++)values[i]=calibration_[i][a];center[a]=med(values,calibrationCount_);}
  for(unsigned i=0;i<calibrationCount_;i++){scores[i]=0;for(unsigned s=0;s<4;s++){float v=0;for(unsigned a=0;a<3;a++){float d=calibration_[i][s*3+a]-center[s*3+a];v+=d*d;}scores[i]=std::max(scores[i],std::sqrt(v));}}
  std::sort(scores,scores+calibrationCount_);float p99=scores[std::min(calibrationCount_-1,unsigned(std::ceil(calibrationCount_*.99f))-1)];
  float drift=0;for(unsigned s=0;s<4;s++){float sum=0;for(unsigned a=0;a<3;a++){for(unsigned i=0;i<40;i++)values[i]=calibration_[i][s*3+a];float first=med(values,40);for(unsigned i=0;i<40;i++)values[i]=calibration_[calibrationCount_-40+i][s*3+a];float d=first-med(values,40);sum+=d*d;}drift=std::max(drift,std::sqrt(sum));}
  if(p99>50||drift>30){calibrationMessage_="Signal was not stable. Remove magnets and try again. Previous reference retained.";return;}
  float off=std::max(20.f,2*p99),on=off*1.5f;Preferences p;
  // One blob update is the commit marker; a model tag prevents incompatible reuse.
  struct Reference {char model[32];float axes[12];float off,on;};Reference ref{};strlcpy(ref.model,phone_model::version,sizeof ref.model);memcpy(ref.axes,center,sizeof center);ref.off=off;ref.on=on;
  if(!p.begin("phone_ref",false)){calibrationMessage_="Cannot save reference. Previous reference retained.";return;}
  bool saved=p.putBytes("ref",&ref,sizeof ref)==sizeof ref;p.end();
  if(!saved){calibrationMessage_="Cannot save reference. Previous reference retained.";return;}
  memcpy(background_,center,sizeof center);off_=off;on_=on;magnet_=false;windowCount_=0;calibrationStatus_="SAVED";customReference_=true;calibrationMessage_="Recalibrated. Put the magnets back to measure. You can recalibrate again any time.";
}
void PhoneProduct::onFrame(const SensorFrame& f){
  PhoneRawFrame r;r.seq=f.seq;r.ts=f.timestampMs;r.healthy=f.validCount==4;
  for(unsigned i=0;i<4;i++){const auto& s=f.sensors[i];r.axes[i*3]=s.raw.x;r.axes[i*3+1]=s.raw.y;r.axes[i*3+2]=s.raw.z;
    r.status[i]=s.status;if(s.valid)r.validMask|=1u<<i;if(s.saturated)r.satMask|=1u<<i;
    r.healthy &=s.valid&&!s.saturated&&s.status==SensorStatus::OK;
    for(unsigned a=0;a<3;a++)r.healthy &=std::isfinite(r.axes[i*3+a]);}
  portENTER_CRITICAL(&mutex_);
  if(queued_==kQueue){overflow_=true;read_=(read_+1)%kQueue;--queued_;}
  queue_[write_]=r;write_=(write_+1)%kQueue;++queued_;
  if(active_ && uint32_t(r.ts-started_)<1000){if(rawCount_<kRaw)raw_[rawCount_++]=r;else failed_=true;}
  portEXIT_CRITICAL(&mutex_);
}
void PhoneProduct::process(const PhoneRawFrame& f){
  if(calibrating_&&uint32_t(f.ts-calibrationStarted_)<5000){if(!f.healthy)++calibrationInvalid_;else if(calibrationCount_<256){if(!calibrationCount_)calibrationFirst_=f.ts;calibrationLast_=f.ts;memcpy(calibration_[calibrationCount_++],f.axes,sizeof f.axes);}}
  if(lastFrame_ && uint32_t(f.ts-lastFrame_)>120){windowCount_=0;magnet_=false;if(active_)failed_=true;}
  lastFrame_=f.ts;lastSeq_=f.seq;
  if(!f.healthy){windowCount_=0;magnet_=false;state_="CHECK_SENSORS";liveReason_="Check all four sensor connections";return;}
  while(windowCount_ && uint32_t(f.ts-window_[0].ts)>200){for(unsigned i=1;i<windowCount_;i++)window_[i-1]=window_[i];--windowCount_;}
  if(windowCount_==16){for(unsigned i=1;i<16;i++)window_[i-1]=window_[i];--windowCount_;}
  window_[windowCount_++]=f;
  if(windowCount_<5||uint32_t(f.ts-window_[0].ts)<160){state_="WARMING_UP";liveReason_="Waiting for a short window of fresh readings";return;}
  if(lastPrediction_ && uint32_t(f.ts-lastPrediction_)<100)return;
  lastPrediction_=f.ts;float magnitudes[16][4],temp[16],filtered[12];
  for(unsigned j=0;j<windowCount_;j++)for(unsigned s=0;s<4;s++){float v=0;for(unsigned a=0;a<3;a++){float x=window_[j].axes[s*3+a];v+=x*x;}magnitudes[j][s]=std::sqrt(v);}
  for(unsigned a=0;a<12;a++){for(unsigned j=0;j<windowCount_;j++)temp[j]=window_[j].axes[a];filtered[a]=med(temp,windowCount_);
    for(unsigned j=0;j<windowCount_;j++)temp[j]=window_[j].axes[a]/std::max(magnitudes[j][a/3],1.f);latestFeatures_[a]=med(temp,windowCount_);}
  for(unsigned s=0;s<4;s++){for(unsigned j=0;j<windowCount_;j++)temp[j]=magnitudes[j][s];latestFeatures_[12+s]=std::log1p(med(temp,windowCount_)/100.f);}
  float signal=0;for(unsigned s=0;s<4;s++){float v=0;for(unsigned a=0;a<3;a++){float d=filtered[s*3+a]-background_[s*3+a];v+=d*d;}signal=std::max(signal,std::sqrt(v));}
  if(signal<=off_)magnet_=false;else if(signal>=on_)magnet_=true;
  if(!magnet_){state_="NO_MAGNET";liveReason_="Position the magnets, or recalibrate with magnets removed";return;}
  float distance,spread;float y=phone_model::predict(latestFeatures_,distance,spread);
  const auto region=range_model::predict(latestFeatures_);
  const bool numericOk=region.cls!=1||(std::isfinite(y)&&y>=-.25f&&y<=8.25f&&distance<=phone_model::supportLimit&&spread<=.75f);
  state_=region.accepted&&numericOk?"READY":"UNSURE";
  liveReason_=region.accepted&&numericOk?"Ready":"Reposition magnets";
  // Judge the median of the full capture, rather than latching a transient
  // model rejection. All raw samples, including invalid ones, are still kept.
  if(active_ && uint32_t(f.ts-started_)<1000 && featureCount_<16){
    if(!featureCount_)featureFirst_=f.ts;featureLast_=f.ts;
    captureValues_[featureCount_]=y;memcpy(captureFeatures_[featureCount_++],latestFeatures_,sizeof latestFeatures_);
  }
}
void PhoneProduct::finish(){
  portENTER_CRITICAL(&mutex_);active_=false;bool lost=failed_;portEXIT_CRITICAL(&mutex_);
  unsigned healthy=0;for(unsigned i=0;i<rawCount_;i++)healthy+=raw_[i].healthy;
  auto unsure=[this](const char* reason){outcome_="UNSURE";state_="UNSURE";belowZero_=false;aboveEight_=false;result_=NAN;category_="Unsure";resultReason_=reason;};
  if(lost||rawCount_<35||millis()-lastFrame_>150){unsure("Sensor data paused. Check the connections and measure again.");return;}
  if(healthy*10<rawCount_*9){unsure("Too many invalid sensor samples. Check all four sensors.");return;}
  if(featureCount_<6||uint32_t(featureLast_-featureFirst_)<600){unsure("Not enough magnet readings. Hold the position for the full second.");return;}
  float f[16],v[16];for(unsigned i=0;i<16;i++){for(unsigned j=0;j<featureCount_;j++)v[j]=captureFeatures_[j][i];f[i]=med(v,featureCount_);}
  const auto region=range_model::capture(f,captureFeatures_,featureCount_);rangeConfidence_=region.confidence;
  if(!region.accepted){unsure("Unfamiliar or changing range pattern. Reposition and measure again.");return;}
  if(region.cls!=1){
    const bool known=(gender_=="male"||gender_=="female")&&(hand_=="right"||hand_=="left");const unsigned index=(gender_=="female"?2:0)+(hand_=="left"?1:0);
    belowZero_=region.cls==0;aboveEight_=region.cls==2;result_=NAN;
    category_=belowZero_?(known&&rules_[index][0]>=0?"Below standard":"Below zero"):(known&&rules_[index][1]<=8?"Above standard":"Above range");
    outcome_="RESULT";state_="RESULT";resultReason_="Learned range classification";return;
  }
  float temporal[16];for(unsigned i=0;i<featureCount_;i++){if(!std::isfinite(captureValues_[i])){unsure("Invalid model reading");return;}temporal[i]=captureValues_[i];}std::sort(temporal,temporal+featureCount_);
  const float variation=temporal[unsigned((featureCount_-1)*.9f)]-temporal[unsigned((featureCount_-1)*.1f)];
  if(variation>.8f){unsure("The position changed during capture. Hold still and measure again.");return;}
  float distance,spread;const float y=phone_model::predict(f,distance,spread);
  if(!std::isfinite(y)||y<-.25f||y>8.25f){unsure("Out of range");return;}
  if(distance>phone_model::supportLimit){unsure("Unfamiliar magnetic pattern. Check magnet orientation and sensor placement.");return;}
  if(spread>.75f){unsure("Model estimates disagree. Reposition the magnets and measure again.");return;}
  belowZero_=false;aboveEight_=false;
  resultReason_="One-second median result. Held until the next measurement.";
  result_=std::round(std::max(0.f,std::min(8.f,y))*100.f)/100.f;outcome_="RESULT";state_="RESULT";category_="—";
  if((gender_=="male"||gender_=="female")&&(hand_=="right"||hand_=="left")){
    unsigned index=(gender_=="female"?2:0)+(hand_=="left"?1:0);float low=rules_[index][0],high=rules_[index][1];
    category_=result_<low?"Below standard":result_>=high?"Above standard":"Standard";
  }
}
void PhoneProduct::update(){
  for(unsigned i=0;i<kQueue;i++){PhoneRawFrame f;bool available;
    portENTER_CRITICAL(&mutex_);available=queued_>0;if(available){f=queue_[read_];read_=(read_+1)%kQueue;--queued_;}bool lost=overflow_;overflow_=false;portEXIT_CRITICAL(&mutex_);
    if(lost){windowCount_=0;if(active_)failed_=true;if(calibrating_)calibrationFailed_=true;}
    if(!available)break;process(f);
  }
  if(active_ && millis()-started_>=1000)finish();
  if(calibrating_&&millis()-calibrationStarted_>=5000)finishCalibration();
  if(lastFrame_ && millis()-lastFrame_>300){state_="CHECK_SENSORS";liveReason_="Sensor readings stopped. Check the connections.";if(active_)failed_=true;}
  if(owner_.length()&&!connected() && (!id_.length()||saved_)){owner_="";name_="";gender_="";hand_="";id_="";result_=NAN;belowZero_=false;aboveEight_=false;category_="—";outcome_="";}
}
void PhoneProduct::status(JsonDocument& d)const{
  d["model"]=range_model::version;d["numericModel"]=phone_model::version;d["rangeConfidence"]=rangeConfidence_;d["state"]=active_?"MEASURING":state_;d["ready"]=!active_&&!calibrating_&&magnet_&&windowCount_>=5&&millis()-lastFrame_<=150;
  d["reason"]=liveReason_;d["resultReason"]=resultReason_;
  d["rangeResult"]=belowZero_?"BELOW_ZERO":aboveEight_?"ABOVE_EIGHT":std::isfinite(result_)?"VALUE":"UNSURE";d["experimental"]=belowZero_||aboveEight_;
  d["calibrationStatus"]=calibrationStatus_;d["referenceSource"]=customReference_?"Recalibrated":"Factory reference";
  d["calibrating"]=calibrating_;d["calibrationRemainingMs"]=calibrating_?5000-std::min(uint32_t(5000),uint32_t(millis()-calibrationStarted_)):0;d["calibrationMessage"]=calibrationMessage_;
  JsonArray rules=d["rules"].to<JsonArray>();uint32_t hash=2166136261u;for(const auto& row:rules_){JsonArray pair=rules.add<JsonArray>();for(float x:row)pair.add(x);}for(unsigned i=0;i<sizeof rules_;i++)hash=(hash^reinterpret_cast<const uint8_t*>(rules_)[i])*16777619u;d["ruleVersion"]=String(hash,HEX);
  static const String boot=String(esp_random(),HEX);d["bootId"]=boot;char device[20];snprintf(device,sizeof device,"%012llX",(unsigned long long)ESP.getEfuseMac());d["deviceId"]=device;
  d["measuring"]=active_;d["remainingMs"]=active_?1000-std::min(uint32_t(1000),uint32_t(millis()-started_)):0;
  d["measurementId"]=id_;d["count"]=rawCount_;d["outcome"]=active_?"MEASURING":outcome_;
  if(std::isfinite(result_))d["resultCm"]=result_;else d["resultCm"]=nullptr;
  d["category"]=category_;d["saved"]=saved_;d["patientName"]=name_;d["gender"]=gender_;d["hand"]=hand_;d["startedMs"]=started_;
}
bool PhoneProduct::command(JsonVariantConst b,JsonDocument& d,String& error){
  const char* action=b["action"]|"";String owner=b["owner"]|"";
  if(owner.length()<16||owner.length()>64){error="Invalid phone session";return false;}
  if(owner_.length()&&owner_!=owner&&(connected()||(id_.length()&&!saved_))){error="Device is in use or waiting for the previous phone to save";return false;}
  owner_=owner;heartbeat_=millis();
  if(!strcmp(action,"connect")||!strcmp(action,"poll")){}
  else if(!strcmp(action,"patient")){
    if(active_){error="Wait until measurement finishes";return false;}
    String name=b["name"]|"";String gender=b["gender"]|"";String hand=b["hand"]|"";String patientId=b["patientId"]|"";
    if(name.length()>240||patientId.length()>64||(gender!=""&&gender!="male"&&gender!="female")||(hand!=""&&hand!="right"&&hand!="left")){error="Invalid patient details";return false;}
    if(id_.length()&&!saved_){error="Save the pending capture on this phone first";return false;}
    if(gender_!=gender||hand_!=hand||patientId_!=patientId){result_=NAN;belowZero_=false;aboveEight_=false;category_="—";id_="";outcome_="";saved_=false;}
    name_=name;gender_=gender;hand_=hand;patientId_=patientId;
  }else if(!strcmp(action,"measure")){
    String id=b["id"]|"";
    if(id.length()<16||id.length()>64){error="Invalid measurement identifier";return false;}
    if(id==id_){status(d);return true;}
    if(id_.length()&&!saved_){error="Save the pending capture before measuring again";return false;}
    if(active_||calibrating_||!magnet_||windowCount_<5||millis()-lastFrame_>150){error="Wait for four healthy sensors and magnet readings";return false;}
    id_=id;saved_=false;featureCount_=0;rangeConfidence_=0;
    portENTER_CRITICAL(&mutex_);started_=millis();rawCount_=0;failed_=false;active_=true;portEXIT_CRITICAL(&mutex_);
  }else if(!strcmp(action,"raw")){
    if(active_||String(b["id"]|"")!=id_){error="Capture unavailable";return false;}
    unsigned offset=b["offset"]|0;JsonArray frames=d["frames"].to<JsonArray>();
    for(unsigned i=offset;i<std::min(offset+8,rawCount_);i++){const auto& f=raw_[i];JsonObject r=frames.add<JsonObject>();r["seq"]=f.seq;r["elapsedMs"]=uint32_t(f.ts-started_);r["healthy"]=f.healthy;r["validMask"]=f.validMask;r["satMask"]=f.satMask;JsonArray values=r["axes"].to<JsonArray>();for(float a:f.axes)values.add(a);JsonArray states=r["states"].to<JsonArray>();for(auto s:f.status)states.add(toString(s));}
    d["count"]=rawCount_;d["measurementId"]=id_;return true;
  }else if(!strcmp(action,"background")){
    if(active_||(id_.length()&&!saved_)){error="Finish and save the current capture first";return false;}
    if(calibrating_){status(d);return true;}
    calibrationCount_=0;calibrationInvalid_=0;calibrationFailed_=false;calibrationStarted_=millis();calibrating_=true;calibrationStatus_="CAPTURING";calibrationMessage_="Remove all magnets and hold sensors still";
  }else if(!strcmp(action,"cancel_background")){
    if(calibrating_){calibrating_=false;calibrationStatus_="CANCELED";calibrationMessage_="Canceled. Previous no-magnet reference kept.";}
  }else if(!strcmp(action,"rules")){
    if(active_||calibrating_||(id_.length()&&!saved_)){error="Finish and save the current capture first";return false;}
    auto values=b["rules"].as<JsonArrayConst>();if(values.size()!=4){error="Four reference rows required";return false;}float next[4][2];
    for(unsigned i=0;i<4;i++){if(!values[i].is<JsonArrayConst>()||values[i].size()!=2){error="Low and high values required";return false;}for(unsigned j=0;j<2;j++){if(!values[i][j].is<float>()){error="Numeric boundaries required";return false;}next[i][j]=values[i][j].as<float>();}if(!std::isfinite(next[i][0])||!std::isfinite(next[i][1])||next[i][0]>=next[i][1]||next[i][0]<-50||next[i][1]>50){error="Invalid reference boundaries";return false;}}
    Preferences p;if(!p.begin("phone_rules",false)){error="Cannot save reference rules";return false;}bool ok=p.putBytes("rules",next,sizeof next)==sizeof next;p.end();if(!ok){error="Rules were not saved";return false;}memcpy(rules_,next,sizeof next);result_=NAN;belowZero_=false;aboveEight_=false;category_="—";id_="";outcome_="";
  }else if(!strcmp(action,"saved")){
    if(String(b["id"]|"")!=id_||active_){error="Capture does not match";return false;}saved_=true;
  }else{error="Unknown command";return false;}
  status(d);return true;
}
}
