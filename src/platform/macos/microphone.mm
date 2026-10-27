/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone/audio capture on macOS.
 * @details Supports:
 *   - System audio capture via the Core Audio process tap (preferred)
 *   - System audio capture via ScreenCaptureKit (macOS 12.3+, fallback)
 *   - Physical microphone capture via AVFoundation
 */
// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_audio.h"
#include "src/platform/macos/sc_audio.h"

namespace platf {
  using namespace std::literals;

  /**
   * @brief macOS microphone capture device and audio format state.
   */
  struct av_mic_t: public mic_t {
    AVAudio *av_audio_capture {};  ///< AV audio capture.

    ~av_mic_t() override {
      [av_audio_capture release];
    }

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_in Sample in.
     * @return Capture status reported to the streaming pipeline.
     */
    capture_e sample(std::vector<float> &sample_in) override {
      const uint32_t neededBytes = static_cast<uint32_t>(sample_in.size() * sizeof(float));
      uint8_t *dst = reinterpret_cast<uint8_t *>(sample_in.data());

      uint32_t remaining = neededBytes;

      while (remaining > 0) {
        uint32_t avail = 0;
        void *tail = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &avail);

        if (avail == 0) {
          // Using 5 second timeout to prevent indefinite hanging
          dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC);
          if (dispatch_semaphore_wait(av_audio_capture->audioSemaphore, timeout) != 0) {
            BOOST_LOG(warning) << "Audio sample timeout - no audio data received within 5 seconds"sv;

            // Fill with silence and return to prevent hanging
            std::fill(sample_in.begin(), sample_in.end(), 0.0f);
            return capture_e::timeout;
          }
          continue;
        }

        const uint32_t toCopy = (avail < remaining) ? avail : remaining;
        std::memcpy(dst, tail, toCopy);

        TPCircularBufferConsume(&av_audio_capture->audioSampleBuffer, toCopy);

        dst += toCopy;
        remaining -= toCopy;
      }

      return capture_e::ok;
    }
  };

  /**
   * @brief System audio capture using ScreenCaptureKit (macOS 12.3+)
   */
  struct sc_mic_t: public mic_t {
    SCAudioCapture *sc_audio_capture API_AVAILABLE(macos(12.3)) {};

    ~sc_mic_t() override {
      if (@available(macOS 12.3, *)) {
        [sc_audio_capture release];
      }
    }

    capture_e sample(std::vector<float> &sample_in) override {
      if (@available(macOS 12.3, *)) {
        const auto neededBytes = static_cast<uint32_t>(sample_in.size() * sizeof(float));

        uint32_t length = 0;
        TPCircularBuffer *buffer = [sc_audio_capture getAudioBuffer];
        void *byteSampleBuffer = TPCircularBufferTail(buffer, &length);

        int wait_count = 0;
        while (length < neededBytes) {
          if (!sc_audio_capture.isCapturing) {
            return capture_e::capture_error;
          }
          [sc_audio_capture.samplesArrivedSignal lock];
          [sc_audio_capture.samplesArrivedSignal waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
          [sc_audio_capture.samplesArrivedSignal unlock];
          byteSampleBuffer = TPCircularBufferTail(buffer, &length);
          // After 500ms of no data, return timeout so the caller can check shutdown_event
          if (++wait_count > 5 && length < neededBytes) {
            return capture_e::timeout;
          }
        }

        std::memcpy(sample_in.data(), byteSampleBuffer, neededBytes);
        TPCircularBufferConsume(buffer, neededBytes);

        return capture_e::ok;
      }
      return capture_e::capture_error;
    }
  };

  /**
   * @brief macOS audio control state used to create microphone streams.
   */
  struct macos_audio_control_t: public audio_control_t {
    AVCaptureDevice *audio_capture_device {};  ///< Audio capture device.

  public:
    /**
     * @brief Update the sink value on the backend.
     *
     * @param sink Audio sink name to route or capture.
     * @return Status from updating sink.
     */
    int set_sink(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::set_sink() unimplemented: "sv << sink;
      return 0;
    }

    /**
     * @brief Create a microphone capture stream for the requested layout.
     *
     * @param mapping Opus channel mapping table for the requested layout.
     * @param channels Number of audio channels in the stream.
     * @param sample_rate Audio sample rate in hertz.
     * @param frame_size Number of samples captured per audio frame.
     * @param continuous_audio Continuous audio.
     * @param host_audio_enabled Whether host playback should remain enabled during capture.
     * @return Microphone capture object for the requested audio layout.
     */
    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, bool host_audio_enabled) override {
      if (is_system_sink(config::audio.sink)) {
        // Use macOS system-wide audio tap
        BOOST_LOG(info) << "Using macOS system audio tap for capture."sv;
        BOOST_LOG(info) << "Sample rate: "sv << sample_rate << ", Frame size: "sv << frame_size << ", Channels: "sv << channels;

        auto mic = std::make_unique<av_mic_t>();
        mic->av_audio_capture = [[AVAudio alloc] init];
        mic->av_audio_capture.hostAudioEnabled = host_audio_enabled ? YES : NO;

        if ([mic->av_audio_capture setupSystemTap:sample_rate frameSize:frame_size channels:channels] == 0) {
          return mic;
        }

        BOOST_LOG(warning) << "Failed to setup system audio tap, trying ScreenCaptureKit."sv;
        if (@available(macOS 12.3, *)) {
          if ([SCAudioCapture isAvailable]) {
            auto sc_mic = std::make_unique<sc_mic_t>();
            sc_mic->sc_audio_capture = [[SCAudioCapture alloc] init];

            if ([sc_mic->sc_audio_capture startCaptureWithSampleRate:sample_rate channels:channels] == 0) {
              BOOST_LOG(info) << "System audio capture enabled via ScreenCaptureKit."sv;
              return sc_mic;
            }
          }
        }

        BOOST_LOG(error) << "No system audio capture available; Screen Recording permission may be missing."sv;
        return nullptr;
      }

      // Use specified macOS audio sink
      const char *audio_sink = config::audio.sink.c_str();
      BOOST_LOG(info) << "Using configured audio sink "sv << audio_sink << " for capture."sv;

      if (!request_microphone_permission()) {
        return nullptr;
      }

      if ((audio_capture_device = [AVAudio findMicrophone:[NSString stringWithUTF8String:audio_sink]]) == nullptr) {
        BOOST_LOG(error) << "opening microphone '"sv << audio_sink << "' failed. Please set a valid input source in the Sunshine config."sv;
        BOOST_LOG(error) << "Available inputs:"sv;

        for (NSString *name in [AVAudio microphoneNames]) {
          BOOST_LOG(error) << "\t"sv << [name UTF8String];
        }

        return nullptr;
      }

      auto mic = std::make_unique<av_mic_t>();
      mic->av_audio_capture = [[AVAudio alloc] init];
      mic->av_audio_capture.hostAudioEnabled = host_audio_enabled ? YES : NO;

      if ([mic->av_audio_capture setupMicrophone:audio_capture_device sampleRate:sample_rate frameSize:frame_size channels:channels]) {
        BOOST_LOG(error) << "Failed to setup microphone."sv;
        return nullptr;
      }

      return mic;
    }

    bool is_sink_available(const std::string &sink) override {
      if (is_system_sink(sink)) {
        return true;
      }

      // Check for physical input devices
      return [AVAudio findMicrophone:[NSString stringWithUTF8String:sink.c_str()]] != nil;
    }

    /**
     * @brief Query host and virtual sink names available to Sunshine.
     *
     * @return Host and virtual sink names when the backend can report them.
     */
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      return sink;
    }

  private:
    static bool is_system_sink(const std::string &sink) {
      return sink.empty() || strcasecmp(sink.c_str(), "system") == 0 ||
             strcasecmp(sink.c_str(), "desktop") == 0 ||
             strcasecmp(sink.c_str(), "screencapturekit") == 0;
    }
  };

  std::unique_ptr<audio_control_t> audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
