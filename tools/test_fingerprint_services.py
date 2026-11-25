"""Exercise actual calibration, measurement and NVS code against host hardware stubs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <cstring>
#include <new>
using String=std::string;
struct IPAddress {};
extern uint32_t testNow;
inline uint32_t millis(){return testNow;}
inline size_t strlcpy(char* out,const char* in,size_t n){size_t len=std::strlen(in);if(n){std::memcpy(out,in,len<n?len:n-1);out[len<n?len:n-1]=0;}return len;}
'''
PREFERENCES = r'''
#pragma once
#include <map>
#include <vector>
#include <string>
#include <cstring>
inline std::map<std::string,std::vector<uint8_t>> testNvs;
inline bool failWrites=false;
class Preferences {
public:
 bool begin(const char*,bool,const char* = nullptr){return true;}
 size_t getBytesLength(const char* k){return testNvs[k].size();}
 size_t putBytes(const char* k,const void* p,size_t n){if(failWrites)return 0;const auto* b=static_cast<const uint8_t*>(p);testNvs[k]={b,b+n};return n;}
 size_t getBytes(const char* k,void* p,size_t n){auto& b=testNvs[k];if(b.size()!=n)return 0;std::memcpy(p,b.data(),n);return n;}
 String getString(const char* k,const char* def){auto& b=testNvs[k];return b.empty()?String(def):String(b.begin(),b.end());}
 size_t putString(const char* k,const String& s){return putBytes(k,s.data(),s.size());}
 bool remove(const char* k){return !failWrites && testNvs.erase(k);}
 bool clear(){if(failWrites)return false;testNvs.clear();return true;}
};
'''
SENSOR = r'''
#pragma once
#include "device_types.h"
namespace scratchsor {
class SensorService {
public:
 SensorFrame frame{};bool have=false;Vec3 restored[4]{};unsigned restores=0;
 bool latest(SensorFrame& out)const{out=frame;return have;}
 void setBaselines(const Vec3* b){for(unsigned i=0;i<4;++i)restored[i]=b[i];++restores;}
 void clearBaselines(){for(auto& x:restored)x={};}
 void tick(float field=5){have=true;++frame.seq;frame.timestampMs=testNow;frame.validCount=4;
   for(unsigned i=0;i<4;++i){auto& r=frame.sensors[i];r.valid=true;r.status=SensorStatus::OK;
     r.saturated=false;r.timestampMs=testNow;r.filtered={field,field+1,field+2};}}
};
}
'''
HARNESS = r'''
#include <cassert>
#include <iostream>
#include "services/storage_service.h"
#include "services/calibration_service.h"
#include "services/measurement_service.h"
#include "services/sensor_service.h"
using namespace scratchsor;
uint32_t testNow=100;
DeviceConfig config(){DeviceConfig c;strlcpy(c.reach.setup,"person-A/mount-1",96);c.reach.selected=true;return c;}
FingerprintData* populated(const DeviceConfig& c){
 auto* d=new FingerprintData{};d->signature=fingerprintSignature(c);d->baselineReady=true;
 for(unsigned i=0;i<12;++i)d->baseline[i]=float(i);
 for(unsigned p=0;p<4;++p)for(unsigned label=0;label<3;++label)for(unsigned j=0;j<10;++j){
   FingerprintPoint point{};point.id=d->nextId++;point.profile=p;point.test=j>=8;
   point.referenceMm=label==0?-50:label==2?100:(p==0?50:p==1?10:p==2?60:30);
   for(auto& v:point.features)v=float(label*100+j);
   assert(appendFingerprint(*d,point));
 }
 for(unsigned p=0;p<4;++p)assert(fitFingerprint(*d,p));return d;
}
int main(int argc,char** argv){
 StorageService storage;storage.begin();const std::string test=argv[1];
 if(test=="migration"){
   struct Old {uint32_t schema;char name[32];bool ap;SensorConfig sensor;FilterConfig filter;OledConfig oled;GapConfig gap;};
   Old old{};old.schema=2;strlcpy(old.name,"preserved",32);old.ap=true;old.sensor.sampleRateHz=12;
   old.filter.baselineRemoval=false;Preferences prefs;prefs.putBytes("cfg",&old,sizeof old);
   assert(storage.saveWifi("home","secret"));DeviceConfig c;assert(storage.loadConfig(c));
   assert(c.schema==3 && c.sensor.sampleRateHz==12 && c.filter.baselineRemoval && !c.reach.selected);
   assert(std::string(c.deviceName)=="preserved");String ssid,pass;assert(storage.loadWifi(ssid,pass));assert(pass=="secret");
   assert(storage.saveConfig(c));DeviceConfig again;assert(storage.loadConfig(again));return 0;
 }
 auto c=config();SensorService sensor;CalibrationService calibration(sensor,storage);calibration.configure(c);calibration.begin();
 if(test=="capture"){
   assert(!calibration.startCollect(0,false));assert(calibration.startBaseline(20));
   for(unsigned i=0;i<20;++i){testNow+=100;sensor.tick(10);calibration.update();}
   assert(calibration.baselineReady() && sensor.restored[0].x==10);
   assert(!calibration.startBaseline(20));assert(!calibration.startCollect(NAN,false));
   assert(calibration.startCollect(0,false));
   for(unsigned i=0;i<19;++i){testNow+=100;sensor.tick(3);calibration.update();}
   assert(calibration.pointCount()==0);testNow+=100;sensor.tick(3);calibration.update();
   assert(calibration.pointCount()==1 && calibration.points()[0].referenceMm==0);
   assert(calibration.startCollect(-25,true));
   for(unsigned i=0;i<20;++i){testNow+=100;sensor.tick(3);calibration.update();}
   assert(calibration.pointCount()==2 && calibration.points()[1].test);
   assert(calibration.startCollect(40,false));
   for(unsigned i=0;i<19;++i){testNow+=100;sensor.tick();calibration.update();}
   testNow+=100;sensor.tick();sensor.frame.sensors[1].saturated=true;calibration.update();
   for(unsigned i=0;i<19;++i){testNow+=100;sensor.tick();calibration.update();}assert(calibration.pointCount()==2);
   testNow+=100;sensor.tick();calibration.update();assert(calibration.pointCount()==3);
   failWrites=true;assert(!calibration.deletePoint(1));assert(calibration.pointCount()==3);
   failWrites=false;assert(calibration.deletePoint(1));assert(calibration.pointCount()==2);
   assert(calibration.startCollect(40,false));testNow+=30001;calibration.update();assert(!calibration.busy() && calibration.pointCount()==2);
   return 0;
 }
 auto* data=populated(c);assert(storage.saveFingerprints(*data));delete data;
 CalibrationService loaded(sensor,storage);loaded.configure(c);loaded.begin();
 assert(loaded.modelValidated() && sensor.restores && sensor.restored[3].z==11);
 if(test=="runtime"){
   MeasurementService measure(sensor,loaded);measure.update();assert(measure.estimate().state==MeasurementState::SENSOR_ERROR);
   for(unsigned i=0;i<19;++i){testNow+=100;sensor.tick();measure.update();assert(measure.estimate().category<0);}
   testNow+=100;sensor.tick();measure.update();assert(measure.estimate().state==MeasurementState::VALID);
   testNow+=1001;measure.update();assert(measure.estimate().state==MeasurementState::SENSOR_ERROR && measure.estimate().category<0);
   for(unsigned i=0;i<19;++i){testNow+=100;sensor.tick();measure.update();assert(measure.estimate().category<0);}
   testNow+=100;sensor.tick();measure.update();assert(measure.estimate().category>=0);
   testNow+=100;sensor.tick();sensor.frame.sensors[0].valid=false;measure.update();assert(measure.estimate().category<0);
   c.reach.profile=1;loaded.configure(c);measure.update();assert(measure.estimate().category<0 && loaded.modelValidated());
   for(unsigned i=0;i<20;++i){testNow+=100;sensor.tick();measure.update();}assert(measure.estimate().profile==1 && measure.estimate().category>=0);
   assert(loaded.deletePoint(31));assert(!loaded.modelValidated());measure.update();assert(measure.estimate().state==MeasurementState::NOT_CALIBRATED);
   c.reach.profile=0;loaded.configure(c);assert(loaded.modelValidated());
   strlcpy(c.reach.setup,"new-mount",96);loaded.configure(c);assert(!loaded.baselineReady() && !loaded.modelValidated());
   assert(!loaded.startCollect(0,false));assert(!loaded.startBaseline());
   assert(loaded.deletePoint(2));assert(!loaded.baselineReady() && !loaded.modelValidated());
   assert(loaded.clearAll());assert(loaded.pointCount()==0);
   return 0;
 }
 if(test=="persistence"){
   auto* copy=new FingerprintData{};assert(storage.loadFingerprints(*copy));assert(copy->count==120);
   auto& blob=testNvs["fingerprint"];blob[blob.size()-1]^=1;assert(!storage.loadFingerprints(*copy));delete copy;
   CalibrationService corrupt(sensor,storage);corrupt.configure(c);corrupt.begin();assert(!corrupt.modelValidated());
   std::cout<<"dataset_bytes="<<sizeof(FingerprintData)<<"\n";
   return 0;
 }
 assert(false);
}
'''


class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name)
        for name, text in {'Arduino.h': ARDUINO, 'Preferences.h': PREFERENCES,
                           'services/sensor_service.h': SENSOR,
                           'diagnostics/diagnostics.h': '#pragma once\n#define SCR_LOGW(...) ((void)0)\n#define SCR_LOGE(...) ((void)0)\n'}.items():
            target = path / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding='utf-8')
        (path / 'services/calibration_service.h').write_text(
            (ROOT / 'include/services/calibration_service.h').read_text(), encoding='utf-8')
        (path / 'services/storage_service.h').write_text(
            (ROOT / 'include/services/storage_service.h').read_text(), encoding='utf-8')
        source = path / 'services.cpp'
        source.write_text(HARNESS, encoding='utf-8')
        compiler = shutil.which('g++') or shutil.which('clang++')
        command = [compiler] if compiler else [sys.executable, '-m', 'ziglang', 'c++']
        cls.exe = path / 'fingerprint_host.exe'
        result = subprocess.run(command + ['-std=c++17', '-I' + str(path), '-I' + str(ROOT / 'include'),
            str(source), *[str(ROOT / ('src/' + name)) for name in ['device_types.cpp',
                'services/fingerprint.cpp', 'services/calibration_service.cpp',
                'services/measurement_service.cpp', 'services/storage_service.cpp']], '-o', str(cls.exe)],
            capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_capture_signed_trials_and_faults(self): self.check('capture')
    def test_profile_switching_stale_faults_and_setup_reset(self): self.check('runtime')
    def test_persistence_reboot_and_crc(self): self.check('persistence')
    def test_config_migration_preserves_wifi(self): self.check('migration')
    def test_dedicated_nvs_keeps_existing_partition_offsets(self):
        partition = ROOT / 'partitions.csv'
        self.assertTrue(partition.exists(), 'Dedicated fingerprint NVS partition missing')
        entries = {row.split(',')[0].strip(): [v.strip() for v in row.split(',')]
                   for row in partition.read_text().splitlines() if row.strip() and not row.startswith('#')}
        self.assertGreaterEqual(int(entries['reach_nvs'][4], 0), 0x10000)
        self.assertEqual(int(entries['nvs'][3], 0), 0x9000)
        self.assertEqual(int(entries['app0'][3], 0), 0x10000)
        self.assertEqual(int(entries['spiffs'][3], 0), 0x290000)

if __name__ == '__main__': unittest.main()
