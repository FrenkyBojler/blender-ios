#pragma once

#include "Audaspace.h"
#include "respec/Specification.h"

#include <memory>
#include <string>
#include <vector>

AUD_NAMESPACE_BEGIN

class AUD_API CaptureDevice {
 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

 public:
  CaptureDevice(const std::string &device_name, DeviceSpecs specs, int buffer_size = AUD_DEFAULT_BUFFER_SIZE);
  ~CaptureDevice();

  CaptureDevice(const CaptureDevice &) = delete;
  CaptureDevice &operator=(const CaptureDevice &) = delete;

  int getAvailableSamples() const;
  int readSamples(int samples, sample_t *buffer);
  DeviceSpecs getSpecs() const;

  static std::vector<std::string> getAvailableInputDeviceNames();
};

AUD_NAMESPACE_END
