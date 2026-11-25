#include "services/measurement_service.h"
#include "services/calibration_service.h"
#include "services/sensor_service.h"
namespace scratchsor {
MeasurementService* gpMeasure=nullptr;
void MeasurementService::update() {
  if(revision_!=calib_.revision()) {revision_=calib_.revision();window_.clear();lastSeq_=0;est_=ClassificationEstimate{};}
  est_.profile=calib_.profile();est_.selected=calib_.selected();est_.ready=calib_.modelValidated();
  auto status=[this](MeasurementState state){window_.clear();est_.category=-1;est_.confidence=0;
    est_.state=state;est_.timestampMs=millis();};
  if(calib_.busy()){status(MeasurementState::CALIBRATING);return;}
  if(!est_.ready){status(MeasurementState::NOT_CALIBRATED);return;}
  SensorFrame frame;float x[kFeatureCount],mean[kFeatureCount];
  if(!sensors_.latest(frame) || !fingerprintFeatures(frame,millis(),x)) {status(MeasurementState::SENSOR_ERROR);return;}
  if(frame.seq==lastSeq_)return;
  if(lastSeq_ && frame.seq!=lastSeq_+1)window_.clear();
  lastSeq_=frame.seq;est_.timestampMs=frame.timestampMs;
  if(!window_.push(x,mean)){est_.category=-1;est_.confidence=0;est_.state=MeasurementState::WARMING_UP;return;}
  est_.category=predictFingerprint(calib_.dataset(),est_.profile,mean,est_.confidence);
  est_.state=est_.category>=0?MeasurementState::VALID:MeasurementState::SENSOR_ERROR;
}
}
