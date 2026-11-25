#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include "networking/dashboard_transport.h"

namespace scratchsor {
namespace {
bool subscribed = false;
uint32_t subscriptionMs = 0;

void writePacket(JsonDocument& packet) {
  String line;
  serializeJson(packet, line);
  line = "@USB " + line;
  line += '\n';
  // One write keeps diagnostics from interleaving inside a protocol record.
  Serial.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.length());
}
}  // namespace

bool usbSubscribed() {
  return subscribed && millis() - subscriptionMs < 5000;
}

void usbBroadcast(const char* json, size_t length) {
  if (!usbSubscribed()) return;
  String line = "@USB {\"event\":";
  line.concat(json, length);
  line += "}\n";
  Serial.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.length());
}

void DashboardTransport::on(const char* path, HTTPMethod method,
                            THandlerFunction handler) {
  WebServer::on(path, method, handler);
  routes_.push_back({path, method, handler});
}

void DashboardTransport::handleClient() {
  WebServer::handleClient();
  if (input_.partial() && millis() - lastByteMs_ > 1000) input_.discard();
  // Bound per-loop work even when another program floods the serial port.
  for (size_t n = 0; n < 512 && Serial.available(); ++n) {
    const char c = static_cast<char>(Serial.read());
    lastByteMs_ = millis();
    if (input_.push(c)) {
      dispatchUsb(input_.line());
      break;
    }
  }
}

void DashboardTransport::dispatchUsb(const char* line) {
  if (strncmp(line, "@USB ", 5)) return;
  JsonDocument request;
  if (deserializeJson(request, line + 5)) return;
  if (request["subscribe"].is<bool>() && request["subscribe"].as<bool>()) {
    subscribed = true;
    subscriptionMs = millis();
    constexpr char ack[] = "{\"t\":\"usb\",\"connected\":true}";
    usbBroadcast(ack, sizeof(ack) - 1);
    return;
  }
  if (!request["id"].is<uint32_t>() || request["id"].as<uint32_t>() == 0 ||
      !request["path"].is<const char*>() ||
      !request["method"].is<const char*>() ||
      !request["body"].is<const char*>() ||
      !request["token"].is<const char*>()) return;

  requestId_ = request["id"].as<uint32_t>();
  token_ = request["token"].as<String>();
  body_ = request["body"].as<String>();
  const char* path = request["path"];
  const char* verb = request["method"];
  const HTTPMethod method = !strcmp(verb, "GET") ? HTTP_GET :
                            !strcmp(verb, "POST") ? HTTP_POST : HTTP_ANY;
  usbRequest_ = true;
  chunked_ = false;
  part_ = 0;
  bool found = false;
  if (method != HTTP_ANY && !strncmp(path, "/api/v1/", 8)) {
    for (const auto& route : routes_) {
      if (route.method == method && !strcmp(route.path, path)) {
        route.handler();
        found = true;
        break;
      }
    }
  }
  if (!found) send(404, "application/json", "{\"error\":\"not_found\"}");
  usbRequest_ = false;
  token_ = "";
  body_ = "";
}

String DashboardTransport::arg(const String& name) {
  return usbRequest_ ? (name == "plain" ? body_ : String()) : WebServer::arg(name);
}

String DashboardTransport::header(const String& name) {
  return usbRequest_ ? (name == "X-SCRATCHSOR-Token" ? token_ : String())
                     : WebServer::header(name);
}

bool DashboardTransport::hasHeader(const String& name) {
  return usbRequest_ ? (name == "X-SCRATCHSOR-Token" && token_.length())
                     : WebServer::hasHeader(name);
}

String DashboardTransport::hostHeader() {
  // Browser Origin/Host checks happen at the loopback bridge trust boundary.
  return usbRequest_ ? String("usb") : WebServer::hostHeader();
}

void DashboardTransport::setContentLength(size_t length) {
  if (usbRequest_) chunked_ = length == CONTENT_LENGTH_UNKNOWN;
  else WebServer::setContentLength(length);
}

void DashboardTransport::send(int code, const char* type, const String& body) {
  if (!usbRequest_) { WebServer::send(code, type, body); return; }
  responseCode_ = code;
  responseType_ = type;
  sendUsb(body, !chunked_);
}

void DashboardTransport::sendContent(const String& body) {
  if (!usbRequest_) { WebServer::sendContent(body); return; }
  sendUsb(body, body.isEmpty());
}

void DashboardTransport::sendUsb(const String& body, bool done) {
  size_t offset = 0;
  do {
    size_t end = offset + 512;
    if (end > body.length()) end = body.length();
    // JSON chunks must each contain valid UTF-8, including Thai setup text.
    while (end < body.length() && end > offset &&
           (static_cast<uint8_t>(body[end]) & 0xC0) == 0x80) --end;
    JsonDocument packet;
    packet["id"] = requestId_;
    packet["part"] = part_++;
    packet["status"] = responseCode_;
    packet["type"] = responseType_;
    packet["body"] = body.substring(offset, end);
    packet["done"] = done && end == body.length();
    writePacket(packet);
    offset = end;
  } while (offset < body.length());
}

}  // namespace scratchsor
