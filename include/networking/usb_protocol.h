#pragma once
#include <cstddef>

namespace scratchsor {

// Overflow/abandoned input is discarded through the next newline, never
// interpreted as a second request containing the tail of the first.
template <size_t Capacity>
class UsbLineBuffer {
public:
  bool push(char c) {
    if (c == '\n') {
      const bool ready = !dropping_ && size_ > 0;
      data_[size_] = '\0';
      size_ = 0;
      dropping_ = false;
      return ready;
    }
    if (!dropping_) {
      if (size_ == Capacity) discard();
      else data_[size_++] = c;
    }
    return false;
  }
  void discard() { size_ = 0; dropping_ = true; }
  bool partial() const { return size_ > 0; }
  const char* line() const { return data_; }

private:
  char data_[Capacity + 1]{};
  size_t size_ = 0;
  bool dropping_ = false;
};

bool usbSubscribed();
void usbBroadcast(const char* json, size_t length);

}  // namespace scratchsor
