#pragma once
#include <WebServer.h>
#include <vector>
#include "networking/usb_protocol.h"

namespace scratchsor {

// Keep the WebServer interface so both transports invoke identical handlers.
class DashboardTransport : public WebServer {
public:
  explicit DashboardTransport(uint16_t port) : WebServer(port) {}
  void on(const char* path, HTTPMethod method, THandlerFunction handler);
  void handleClient();
  String arg(const String& name);
  String header(const String& name);
  bool hasHeader(const String& name);
  String hostHeader();
  void send(int code, const char* type, const String& body);
  void sendContent(const String& body);
  void setContentLength(size_t length);

private:
  struct Route { const char* path; HTTPMethod method; THandlerFunction handler; };
  std::vector<Route> routes_;
  UsbLineBuffer<4096> input_;
  uint32_t lastByteMs_ = 0, requestId_ = 0, part_ = 0;
  bool usbRequest_ = false, chunked_ = false;
  int responseCode_ = 200;
  String token_, body_, responseType_;

  void dispatchUsb(const char* line);
  void sendUsb(const String& body, bool done);
};

}  // namespace scratchsor
