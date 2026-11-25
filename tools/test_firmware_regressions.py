"""Host checks for firmware methods, with hardware boundaries replaced by fixtures.

Compile the actual recorder method and reset handler from source so these checks
exercise their side effects without an ESP32. Requires a C++ compiler and the
ArduinoJson dependency installed by PlatformIO.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler:
            command = [compiler]
        elif importlib.util.find_spec("ziglang"):
            command = [sys.executable, "-m", "ziglang", "c++"]
        else:
            raise unittest.SkipTest("Install a C++ compiler or ziglang to run firmware host checks")
        json_include = ROOT / ".pio/libdeps/esp32dev/ArduinoJson/src"
        if not json_include.exists():
            raise unittest.SkipTest("Run pio pkg install -e esp32dev for ArduinoJson")
        sensor_source = (ROOT / "src/services/sensor_service.cpp").read_text(encoding="utf-8")
        append = sensor_source.split('    haveFrame_ = true;', 1)[1].split('    portEXIT_CRITICAL', 1)[0]
        sensor_header = (ROOT / "include/services/sensor_service.h").read_text(encoding="utf-8")
        active = sensor_header.split('bool recording() const {', 1)[1].split('}', 1)[0]
        start = 'bool recordStart(' + sensor_source.split('bool SensorService::recordStart(', 1)[1].split('\n}', 1)[0] + '\n}'
        recorder = sensor_source
        recorder = recorder.split("uint16_t SensorService::recordCopy(", 1)[1].split("\n}", 1)[0]
        recorder = "uint16_t recordCopy(" + recorder + "\n}"
        recorder = recorder.replace("uint16_t offset)", "uint16_t offset = 0)")
        routes = (ROOT / "src/networking/web_server.cpp").read_text(encoding="utf-8")
        record_route = routes.split('server_.on("/api/v1/record", HTTP_POST, [this] {', 1)[1].split('\n  });', 1)[0]
        csv_line = 'snprintf(line, sizeof(line),' + routes.split('snprintf(line, sizeof(line),', 1)[1].split('          buf += line;', 1)[0]
        reference = (ROOT / 'src/services/fingerprint.cpp').read_text(encoding='utf-8').split('bool referenceAllowed(', 1)[1].split('\n', 1)[0]
        oled = (ROOT / 'src/hardware/oled_manager.cpp').read_text(encoding='utf-8')
        oled_summary = 'void renderLiveSummary(' + oled.split('void OledManager::renderLiveSummary(', 1)[1].split('\n}', 1)[0] + '\n}'
        reset = routes.split('server_.on("/api/v1/factory_reset", HTTP_POST, [this] {', 1)[1]
        reset = reset.split("\n  });", 1)[0]
        frame_append = "if (!first) out += ',';" + routes.split("if (!first) out += ',';", 1)[1].split("\n      }", 1)[0]
        transport = (ROOT / "src/networking/dashboard_transport.cpp").read_text(encoding="utf-8")
        packet_writer = "void writePacket(" + transport.split("void writePacket(", 1)[1].split("\n}", 1)[0] + "\n}"
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <iostream>
#include <cstdio>
#include <vector>
#include <memory>
#include <ArduinoJson.h>
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define SCR_LOGW(...) ((void)0)
using String = std::string;
struct SerialOutput {
  String data;
  void write(const uint8_t* p, size_t n) { data.assign(reinterpret_cast<const char*>(p), n); }
} Serial;
PACKET_WRITER
constexpr uint16_t kRecordMaxFrames = 4;
constexpr uint32_t kRecordDurationMs = 3000;
uint32_t testNow = 100;
uint32_t millis() { return testNow; }
constexpr unsigned kSensorCount = 4;
enum class SensorStatus { OK, TIMEOUT };
struct OledReading { bool valid=false; SensorStatus status=SensorStatus::OK; bool saturated=false; uint32_t timestampMs=100; };
struct SensorFrame { unsigned seq = 0; uint32_t timestampMs = 0; unsigned validCount = 1; OledReading sensors[4]; };
constexpr int u8g2_font_7x13_tf=0, u8g2_font_5x8_tf=1;
struct Display {
  std::vector<String> text;unsigned filled=0,hollow=0;
  void clearBuffer(){text.clear();filled=hollow=0;}
  void setFont(int){}
  void drawStr(unsigned,unsigned,const char* s){text.emplace_back(s);}
  void drawDisc(unsigned,unsigned,unsigned){++filled;}
  void drawCircle(unsigned,unsigned,unsigned){++hollow;}
  void sendBuffer(){}
};
void drawHeader(Display* d,const char* s){d->drawStr(0,0,s);}
struct Oled {
  std::unique_ptr<Display> disp_{new Display};SensorFrame frame_;
  struct {float measuredRateHz=50;} health_;
  OLED_SUMMARY
};
bool referenceAllowed(REFERENCE
struct Recorder {
  SensorFrame frames[4]{{12}, {13}, {10}, {11}};
  SensorFrame* recBuf_ = frames;
  uint16_t recHead_ = 2, recCount_ = 4;
  bool recording_ = false;
  uint32_t recStartedMs_ = 100;
  float recReferenceMm_ = 0;
  SensorFrame latestFrame{1, 100};
  bool haveFrame = true;
  bool latest(SensorFrame& out) const { out = latestFrame; return haveFrame; }
  unsigned recordCount() const { return recCount_; }
  float recordReferenceMm() const { return recReferenceMm_; }
  uint32_t recordStartedMs() const { return recStartedMs_; }
  uint32_t recordRemainingMs() const { return recording() ? 3000 - (testNow - recStartedMs_) : 0; }
  void recordStop() { recording_ = false; }
  bool recording() const { ACTIVE }
  void append(const SensorFrame& f) { APPEND }
  START
  RECORDER
};
uint32_t sRestartAtMs = 0;
struct Storage {
  int keep = 0, all = 0;
  bool factoryResetKeepWifi() { ++keep; return true; }
  bool factoryResetAll() { ++all; return true; }
};
struct Calibration { bool active = false; bool busy() const { return active; } };
struct Ws { void broadcastRecord(bool, unsigned, unsigned) {} } gWs;
struct Handler {
  struct { Storage* storage; Recorder* sensors = nullptr; Calibration* calib = nullptr; } svc_;
  struct Server {
    String body;
    String arg(const char*) { return body; }
  } server_;
  int status = 0;
  bool requireAuth() { return true; }
  void sendError(int code, const char*, const char*) { status = code; }
  void sendJson(int code, const char*) { status = code; }
  void reset() { RESET
  }
  void record() { RECORD_ROUTE
  }
};
int main(int argc, char** argv) {
  const std::string test = argv[1];
  if (test == "packet") {
    JsonDocument packet;
    packet["id"] = 1;
    writePacket(packet);
    assert(Serial.data == "@USB {\"id\":1}\n");
  } else if (test == "json_frames") {
    String out;
    bool first = true;
    for (int i = 1; i <= 2; ++i) {
      JsonDocument fd;
      fd["seq"] = i;
      FRAME_APPEND
    }
    JsonDocument result;
    assert(!deserializeJson(result, "[" + out + "]"));
    assert(result.size() == 2 && result[0]["seq"] == 1 && result[1]["seq"] == 2);
  } else if (test == "oled") {
    Oled oled;
    auto contains=[&](const char* text){for(const auto& s:oled.disp_->text)if(s==text)return true;return false;};
    oled.renderLiveSummary();assert(contains("0/4 sensors online") && oled.disp_->hollow==4);
    for(auto& r:oled.frame_.sensors)r.valid=true;
    oled.renderLiveSummary();assert(contains("4/4 sensors online") && contains("Rate: 50.0 Hz") && oled.disp_->filled==4);
    oled.frame_.sensors[0].saturated=true;oled.frame_.sensors[1].status=SensorStatus::TIMEOUT;
    oled.renderLiveSummary();assert(contains("2/4 sensors online") && oled.disp_->filled==2);
    testNow=1200;oled.renderLiveSummary();assert(contains("0/4 sensors online") && oled.disp_->hollow==4);
  } else if (test == "record_route") {
    Storage storage;Recorder r;Calibration cal;
    Handler h{{&storage, &r, &cal}, {argv[2]}};
    const int expected = std::stoi(argv[3]);
    cal.active = argc > 4;
    h.record();assert(h.status == expected);
    if (expected != 200) assert(r.recCount_ == 4 && !r.recording_);
    else if (r.recording()) assert(r.recCount_ == 0);
  } else if (test == "csv") {
    Recorder recorder;recorder.recReferenceMm_ = -25;recorder.recStartedMs_ = 100;
    struct { Recorder* sensors; } svc_{&recorder};
    struct Vec { double x = 0.123456791, y = -12345.125, z = 3; };
    struct Reading { Vec raw, filtered; double magnitudeFiltered = 5, magnitudeRaw = 6;
      int status = 0; bool valid = false, saturated = true; double temperatureC = 25, quality = 0.25; } r;
    auto toString = [](int) { return "SATURATED"; };
    SensorFrame f{42, 120};unsigned i = 3;char line[384];
    CSV_LINE
    std::cout << line;
  } else if (test == "start") {
    Recorder r;
    assert(!r.recordStart(NAN) && !r.recordStart(INFINITY) && !r.recordStart(501));
    assert(r.recCount_ == 4);
    r.haveFrame = false;assert(!r.recordStart(25));
    r.haveFrame = true;r.latestFrame.validCount = 0;assert(!r.recordStart(25));
    r.latestFrame.validCount = 1;testNow = 1200;assert(!r.recordStart(25));
    testNow = 100;assert(r.recordStart(-25));
    assert(r.recording() && r.recCount_ == 0 && r.recReferenceMm_ == -25);
    r.append({99, 110});
    assert(!r.recordStart(30) && r.recReferenceMm_ == -25 && r.recCount_ == 1);
    testNow = 3100;r.latestFrame.timestampMs = testNow;
    assert(r.recordStart(0) && r.recReferenceMm_ == 0 && r.recCount_ == 0);
    r.recording_ = false;r.recBuf_ = nullptr;r.recCount_ = 2;
    assert(!r.recordStart(25) && r.recCount_ == 2);
  } else if (test == "window") {
    Recorder r;
    r.recHead_ = r.recCount_ = 0;
    r.recording_ = true;
    r.append({1, 99}); // A conversion already in flight when Record was pressed.
    assert(r.recCount_ == 0);
    testNow = 3099;
    r.append({2, 3090});
    assert(r.recording() && r.recCount_ == 1);
    testNow = 3100;
    assert(!r.recording());
    r.append({3, 3100});
    assert(r.recCount_ == 1);
    SensorFrame out[4];
    assert(r.recordCopy(out, 4) == 1 && out[0].seq == 2);
  } else if (test == "capacity") {
    Recorder r;
    r.recording_ = true;
    r.append({99, 100});
    assert(!r.recording());
    SensorFrame out[4];
    assert(r.recordCopy(out, 4) == 4 && out[0].seq == 10 && out[3].seq == 13);
  } else if (test == "wrap") {
    Recorder r;
    r.recStartedMs_ = UINT32_MAX - 99;
    r.recCount_ = r.recHead_ = 0;
    r.recording_ = true;
    testNow = 100;
    r.append({1, 100});
    assert(r.recording() && r.recCount_ == 1);
    testNow = 2900;
    assert(!r.recording());
  } else if (test == "recorder") {
    Recorder r;
    SensorFrame out[4];
    assert(r.recordCopy(out, 2) == 2);
    assert(out[0].seq == 10 && out[1].seq == 11);
    assert(r.recCount_ == 4 && r.recHead_ == 2);
    assert(r.recordCopy(out, 4) == 4);
    assert(out[0].seq == 10 && out[3].seq == 13);
    assert(r.recordCopy(nullptr, 4) == 0);
    assert(r.recordCopy(out, 0) == 0);
    r.recBuf_ = nullptr;
    assert(r.recordCopy(out, 4) == 0);
  } else if (test == "pages") {
    Recorder r;
    SensorFrame out[4];
    assert(r.recordCopy(out, 4, 2) == 2);
    assert(out[0].seq == 12 && out[1].seq == 13);
    assert(r.recordCopy(out, 4, 4) == 0);
    assert(r.recordCopy(out, 4, 65535) == 0);
    r.recording_ = true;
    assert(r.recordCopy(out, 4) == 0 && r.recCount_ == 4);
    r.recording_ = false;
    r.recCount_ = 0;
    assert(r.recordCopy(out, 4) == 0);
  } else {
    Storage storage;
    Handler h{{&storage}, {argc > 2 ? argv[2] : ""}};
    h.reset();
    if (test == "invalid") {
      assert(h.status == 400);
      assert(storage.keep == 0 && storage.all == 0 && sRestartAtMs == 0);
    } else {
      assert(h.status == 200 && sRestartAtMs > 0);
      assert(storage.keep == (test == "keep" ? 1 : 0));
      assert(storage.all == (test == "all" ? 1 : 0));
    }
  }
}
'''.replace("RECORDER", recorder).replace("ACTIVE", active).replace("FRAME_APPEND", frame_append).replace("APPEND", append).replace("START", start).replace("RESET", reset).replace("PACKET_WRITER", packet_writer).replace("RECORD_ROUTE", record_route).replace("REFERENCE", reference).replace("CSV_LINE", csv_line).replace("OLED_SUMMARY", oled_summary)
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "regressions.cpp"
        source.write_text(harness, encoding="utf-8")
        cls.exe = source.with_suffix(".exe" if os.name == "nt" else "")
        built = subprocess.run(command + ["-std=c++14", "-I" + str(json_include), str(source),
                                          "-o", str(cls.exe)], capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(built.stdout + built.stderr)

    def run_case(self, *args):
        result = subprocess.run([str(self.exe), *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_export_copy_retains_recording_for_retry_and_second_format(self):
        self.run_case("recorder")

    def test_three_second_window_excludes_in_flight_and_late_frames(self):
        self.run_case("window")

    def test_oled_raw_status_uses_fresh_online_sensor_count(self):
        self.run_case('oled')

    def test_record_start_validates_labels_hardware_and_busy_without_clearing_data(self):
        self.run_case("start")

    def test_record_api_rejects_invalid_labels_shapes_and_calibration_overlap(self):
        for body in ('', 'null', '[]', '{}', '{"active":"true","refMm":25}',
                     '{"active":true}', '{"active":true,"refMm":null}',
                     '{"active":true,"refMm":"25"}', '{"active":true,"refMm":501}'):
            with self.subTest(body=body):
                self.run_case('record_route', body, '400')
        self.run_case('record_route', '{"active":true,"refMm":-25}', '409', 'calibration')
        for body in ('{"active":true,"refMm":-25}', '{"active":true,"refMm":0}', '{"active":false}'):
            self.run_case('record_route', body, '200')

    def test_csv_formatter_preserves_training_label_precision_and_sensor_flags(self):
        import csv
        result = subprocess.run([str(self.exe), 'csv'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        row, = list(csv.reader(result.stdout.splitlines()))
        self.assertEqual(len(row), 15)
        self.assertEqual(row[:3], ['42', '120', '3'])
        self.assertAlmostEqual(float(row[3]), 0.123456791, places=9)
        self.assertEqual(row[7:13], ['SATURATED', '-25', '20', '100', '0', '1'])

    def test_full_recorder_preserves_first_frames(self):
        self.run_case("capacity")

    def test_recording_duration_survives_millis_wrap(self):
        self.run_case("wrap")

    def test_firmware_response_keeps_usb_prefix(self):
        self.run_case("packet")

    def test_json_export_keeps_every_frame_and_comma(self):
        self.run_case("json_frames")

    def test_export_pages_wrap_and_bound_offsets_and_require_stopped_recording(self):
        self.run_case("pages")

    def test_malformed_reset_never_erases_or_reboots(self):
        self.run_case("invalid", '{"keepWifi":')

    def test_reset_rejects_invalid_body_shapes_and_types(self):
        for body in ('[]', 'null', '{"keepWifi":"false"}', '{"keepWifi":null}'):
            with self.subTest(body=body):
                self.run_case("invalid", body)

    def test_valid_reset_preserves_keep_wifi_choice(self):
        for body in ("", "{}", '{"keepWifi":true}'):
            self.run_case("keep", body)
        self.run_case("all", '{"keepWifi":false}')


if __name__ == "__main__":
    unittest.main()
