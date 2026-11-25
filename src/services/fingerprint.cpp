#include "services/fingerprint.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace scratchsor {
int reachCategory(uint8_t profile, float mm) {
  if (profile >= 4 || !std::isfinite(mm)) return -1;
  const float lower[] = {40, 0, 50, 0}, upper[] = {60, 30, 70, 60};
  return mm < lower[profile] ? 0 : mm < upper[profile] ? 1 : 2;
}
bool referenceAllowed(float mm) { return std::isfinite(mm) && mm >= -500 && mm <= 500; }
const char* categoryName(int c) {
  const char* names[] = {"Below Standard", "Standard", "Above Standard"};
  return c >= 0 && c < 3 ? names[c] : "";
}
const char* profileName(uint8_t p) {
  const char* names[] = {"Male / Right on top", "Male / Left on top",
                         "Female / Right on top", "Female / Left on top"};
  return p < 4 ? names[p] : "Select profile";
}
bool appendFingerprint(FingerprintData& d, const FingerprintPoint& p) {
  if (d.count >= kFingerprintCapacity || p.profile >= 4 || !p.id || !referenceAllowed(p.referenceMm)) return false;
  for (float x : p.features) if (!std::isfinite(x)) return false;
  for (unsigned i=0; i<d.count; ++i) if (d.points[i].id == p.id) return false;
  d.points[d.count++] = p;
  d.models[p.profile].ready = false;
  return true;
}
bool deleteFingerprint(FingerprintData& d, uint32_t id) {
  for (unsigned i=0; i<d.count; ++i) if (d.points[i].id == id) {
    d.models[d.points[i].profile].ready = false;
    std::memmove(&d.points[i], &d.points[i+1], (--d.count-i)*sizeof(FingerprintPoint));
    return true;
  }
  return false;
}
namespace {
int nearest(const FingerprintData& d, uint8_t profile, const float* x, float& vote) {
  const auto& m = d.models[profile];
  double distances[3] = {INFINITY, INFINITY, INFINITY};
  int labels[3] = {-1,-1,-1};
  for (unsigned j=0; j<kFeatureCount; ++j) if (!std::isfinite(x[j])) return -1;
  for (unsigned i=0; i<d.count; ++i) {
    const auto& p=d.points[i];
    if (p.profile != profile || p.test) continue;
    double distance=0;
    for (unsigned j=0; j<kFeatureCount; ++j) {
      const double delta = (double(x[j])-p.features[j])/m.scale[j];
      distance += delta*delta;
    }
    for (unsigned k=0; k<3; ++k) if (distance < distances[k]) {
      for (unsigned n=2; n>k; --n) {distances[n]=distances[n-1];labels[n]=labels[n-1];}
      distances[k]=distance; labels[k]=reachCategory(profile,p.referenceMm); break;
    }
  }
  if (labels[2]<0) return -1;
  unsigned counts[3]{};
  for (int c:labels) ++counts[c];
  int best=labels[0];
  for (int c:labels) if(counts[c]>counts[best]) best=c;
  vote=counts[best]/3.f;
  return best;
}
}
int predictFingerprint(const FingerprintData& d, uint8_t p, const float* x, float& vote) {
  vote=0;
  return p<4 && d.models[p].ready ? nearest(d,p,x,vote) : -1;
}
bool fitFingerprint(FingerprintData& d, uint8_t profile) {
  if (profile>=4) return false;
  auto& m=d.models[profile]; m=FingerprintModel{};
  unsigned count=0;
  double sums[kFeatureCount]{}, squares[kFeatureCount]{};
  for (unsigned i=0; i<d.count; ++i) {
    const auto& p=d.points[i]; if(p.profile!=profile)continue;
    const int c=reachCategory(profile,p.referenceMm);
    if (p.test) {++m.testCount[c];continue;}
    ++m.trainCount[c];++count;
    for(unsigned j=0;j<kFeatureCount;++j) {sums[j]+=p.features[j];squares[j]+=double(p.features[j])*p.features[j];}
  }
  for(unsigned c=0;c<3;++c) if(m.trainCount[c]<8 || m.testCount[c]<2) return false;
  for(unsigned j=0;j<kFeatureCount;++j) {
    m.mean[j]=float(sums[j]/count);
    const double variance=squares[j]/count-(sums[j]/count)*(sums[j]/count);
    m.scale[j]=variance>1e-12?float(std::sqrt(variance)):1.f;
  }
  unsigned correct=0,total=0;
  for(unsigned i=0;i<d.count;++i) {
    const auto& p=d.points[i];if(p.profile!=profile || !p.test)continue;
    float vote;const int predicted=nearest(d,profile,p.features,vote);
    if(predicted<0)return false;
    const int actual=reachCategory(profile,p.referenceMm);
    ++m.confusion[actual][predicted];++total;correct+=actual==predicted;
  }
  m.accuracy=float(correct)/total;
  for(unsigned c=0;c<3;++c)m.recall[c]=float(m.confusion[c][c])/m.testCount[c];
  m.ready=true;
  return true;
}
bool validFingerprintData(const FingerprintData& d) {
  if(d.count>kFingerprintCapacity || !d.nextId)return false;
  for(float x:d.baseline)if(!std::isfinite(x))return false;
  for(unsigned i=0;i<d.count;++i){const auto& p=d.points[i];
    if(!p.id || p.id>=d.nextId || p.profile>=4 || !referenceAllowed(p.referenceMm))return false;
    for(float x:p.features)if(!std::isfinite(x))return false;
    for(unsigned j=0;j<i;++j)if(d.points[j].id==p.id)return false;
  }
  return true;
}
void FeatureWindow::clear() {count_=head_=0;std::memset(sums_,0,sizeof sums_);}
bool FeatureWindow::push(const float* x,float* mean) {
  for(unsigned j=0;j<kFeatureCount;++j)if(!std::isfinite(x[j])){clear();return false;}
  for(unsigned j=0;j<kFeatureCount;++j){
    if(count_==kFeatureFrames)sums_[j]-=frames_[head_][j];
    sums_[j]+=x[j];frames_[head_][j]=x[j];
  }
  if(count_<kFeatureFrames)++count_;
  head_=(head_+1)%kFeatureFrames;
  for(unsigned j=0;j<kFeatureCount;++j)mean[j]=float(sums_[j]/count_);
  return count_==kFeatureFrames;
}
} // namespace scratchsor
