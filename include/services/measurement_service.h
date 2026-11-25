#pragma once
#include "device_types.h"
namespace scratchsor {
class SensorService;
class CalibrationService;
class MeasurementService {
public:
  MeasurementService(SensorService& sensors,CalibrationService& calib):sensors_(sensors),calib_(calib){}
  void update();
  const ClassificationEstimate& estimate() const {return est_;}
private:
  SensorService& sensors_;
  CalibrationService& calib_;
  ClassificationEstimate est_{};
  FeatureWindow window_;
  uint32_t lastSeq_=0,revision_=UINT32_MAX;
};
extern MeasurementService* gpMeasure;
}
