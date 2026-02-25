#include "devices/CaptureDevice.h"

#include "Exception.h"
#include "respec/ConverterFunctions.h"

#ifdef WITH_OPENAL
#  include <al.h>
#  include <alc.h>
#  include <algorithm>
#  include <cstring>
#endif

AUD_NAMESPACE_BEGIN

struct CaptureDevice::Impl {
#ifdef WITH_OPENAL
  ALCdevice *device = nullptr;
#endif
  DeviceSpecs specs{};
};

#ifdef WITH_OPENAL

CaptureDevice::CaptureDevice(const std::string &device_name, DeviceSpecs specs, int buffer_size)
    : m_impl(std::make_unique<Impl>())
{
  m_impl->specs = specs;

  if ((specs.channels != CHANNELS_MONO) && (specs.channels != CHANNELS_STEREO)) {
    specs.channels = CHANNELS_MONO;
    m_impl->specs.channels = CHANNELS_MONO;
  }
  if (specs.format == FORMAT_INVALID) {
    specs.format = FORMAT_FLOAT32;
    m_impl->specs.format = FORMAT_FLOAT32;
  }

  const char *device_name_c = device_name.empty() ? nullptr : device_name.c_str();
  /* Use a safe 1-second capture buffer instead of the default UI playback buffer */
  const ALCsizei capture_buffer_size = specs.rate * specs.channels * 2;
  m_impl->device = alcCaptureOpenDevice(device_name_c,
                                        specs.rate,
                                        specs.channels == CHANNELS_MONO ? AL_FORMAT_MONO16 :
                                                                           AL_FORMAT_STEREO16,
                                        capture_buffer_size);
  if (!m_impl->device) {
    AUD_THROW(DeviceException, "The capture device couldn't be opened with OpenAL.");
  }

  alcCaptureStart(m_impl->device);
}

CaptureDevice::~CaptureDevice()
{
  if (m_impl && m_impl->device) {
    alcCaptureStop(m_impl->device);
    alcCaptureCloseDevice(m_impl->device);
    m_impl->device = nullptr;
  }
}

int CaptureDevice::getAvailableSamples() const
{
  if (!m_impl || !m_impl->device) {
    return 0;
  }

  int length = 0;
  alcGetIntegerv(m_impl->device, ALC_CAPTURE_SAMPLES, 1, &length);
  return length;
}

int CaptureDevice::readSamples(const int samples, sample_t *buffer)
{
  if (!m_impl || !m_impl->device || samples <= 0 || buffer == nullptr) {
    return 0;
  }

  const int available = getAvailableSamples();
  const int to_read = std::min(samples, available);
  if (to_read <= 0) {
    return 0;
  }

  /* Prevent memory overwrite: OpenAL writes 16-bit integers, buffer is 32-bit float. */
  std::vector<int16_t> temp_buffer(to_read * m_impl->specs.channels);
  alcCaptureSamples(m_impl->device, temp_buffer.data(), to_read);
  convert_s16_float((data_t *)buffer, (data_t *)temp_buffer.data(), to_read * m_impl->specs.channels);

  return to_read;
}

std::vector<std::string> CaptureDevice::getAvailableInputDeviceNames()
{
  std::vector<std::string> names;

  const ALCchar *devices = alcGetString(nullptr, ALC_CAPTURE_DEVICE_SPECIFIER);
  if (devices != nullptr) {
    const ALCchar *cursor = devices;
    while (*cursor != '\0') {
      names.emplace_back(cursor);
      cursor += std::strlen(cursor) + 1;
    }
  }

  const ALCchar *default_device = alcGetString(nullptr, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
  if (default_device && default_device[0] != '\0') {
    const std::string default_name(default_device);
    if (std::find(names.begin(), names.end(), default_name) == names.end()) {
      names.insert(names.begin(), default_name);
    }
  }

  return names;
}

#else

CaptureDevice::CaptureDevice(const std::string &device_name, DeviceSpecs specs, int buffer_size)
    : m_impl(std::make_unique<Impl>())
{
  (void)device_name;
  m_impl->specs = specs;
  (void)buffer_size;
  AUD_THROW(DeviceException, "Capture is not available in this Audaspace build.");
}

CaptureDevice::~CaptureDevice() {}

int CaptureDevice::getAvailableSamples() const
{
  return 0;
}

int CaptureDevice::readSamples(const int samples, sample_t *buffer)
{
  (void)samples;
  (void)buffer;
  return 0;
}

std::vector<std::string> CaptureDevice::getAvailableInputDeviceNames()
{
  return {};
}

#endif

DeviceSpecs CaptureDevice::getSpecs() const
{
  return m_impl ? m_impl->specs : DeviceSpecs{};
}

AUD_NAMESPACE_END
