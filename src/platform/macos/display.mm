/**
 * @file src/platform/macos/display.mm
 * @brief Definitions for display capture on macOS.
 */

// standard includes
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// local includes
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/av_video.h"
#include "src/platform/macos/misc.h"
#include "src/platform/macos/nv12_zero_device.h"
#import "src/platform/macos/sc_capture.h"

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
/**
 * @def AVMediaType
 * @brief Macro for AV media type.
 */
#define AVMediaType AVMediaType_FFmpeg
#include "src/video.h"
#undef AVMediaType

namespace platf {
  using namespace std::literals;

  namespace {
    std::optional<CGDirectDisplayID> parse_display_id(std::string_view display_name) {
      if (display_name.empty()) {
        return std::nullopt;
      }

      CGDirectDisplayID display_id {};
      const auto *const begin {display_name.data()};
      const auto *const end {display_name.data() + display_name.size()};
      const auto [ptr, ec] {std::from_chars(begin, end, display_id)};
      if (ec != std::errc {} || ptr != end) {
        return std::nullopt;
      }

      return display_id;
    }

    /**
     * @brief Return the union of active CoreGraphics display bounds.
     *
     * @param fallback_display Display whose bounds are used if enumeration fails.
     * @return Full active desktop bounds in CoreGraphics coordinates.
     */
    CGRect active_desktop_bounds(CGDirectDisplayID fallback_display) {
      std::uint32_t display_count {};
      if (CGGetActiveDisplayList(0, nullptr, &display_count) != kCGErrorSuccess || display_count == 0) {
        return CGDisplayBounds(fallback_display);
      }

      std::vector<CGDirectDisplayID> displays(display_count);
      if (CGGetActiveDisplayList(display_count, displays.data(), &display_count) != kCGErrorSuccess || display_count == 0) {
        return CGDisplayBounds(fallback_display);
      }

      auto bounds = CGDisplayBounds(displays.front());
      for (std::uint32_t index = 1; index < display_count; ++index) {
        bounds = CGRectUnion(bounds, CGDisplayBounds(displays[index]));
      }
      return bounds;
    }

    OSType videotoolbox_pixel_format(const video::config_t &config) {
      const auto colorspace {video::colorspace_from_client_config(config, false)};
      return colorspace.bit_depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    }
  }  // namespace

  /**
   * @brief Process a CMSampleBuffer frame into an img_t for the encoder pipeline.
   * Shared between AVFoundation and ScreenCaptureKit capture backends.
   */
  static bool process_frame(CMSampleBufferRef sampleBuffer, img_t *img) {
    // Check for valid pixel data before processing (SCK can deliver status frames without image content)
    CVPixelBufferRef pixBuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixBuf) {
      return false;
    }

    auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
    auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

    auto av_img = (av_img_t *) img;

    auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
      av_img->sample_buffer,
      av_img->pixel_buffer,
      img->data
    );

    av_img->sample_buffer = new_sample_buffer;
    av_img->pixel_buffer = new_pixel_buffer;
    img->data = new_pixel_buffer->data();

    img->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
    img->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
    img->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
    img->pixel_pitch = img->row_pitch / img->width;

    old_data_retainer = nullptr;
    return true;
  }

  /**
   * @brief macOS display capture source and image buffers, AVFoundation backend.
   */
  struct av_display_t: public display_t {
    AVVideo *av_capture {};  ///< AV capture.
    CGDirectDisplayID display_id {};  ///< Display ID.
    std::unique_ptr<display_device::DisplayPowerGuardInterface> display_power_guard;  ///< Display power guard.

    ~av_display_t() override {
      [av_capture release];
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto signal = [av_capture capture:^(CMSampleBufferRef sampleBuffer) {
        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return false;
        }
        process_frame(sampleBuffer, img_out.get());
        if (!push_captured_image_cb(std::move(img_out), true)) {
          return false;
        }
        return true;
      }];

      // Poll with timeout instead of waiting forever, so the capture thread
      // can respond to session shutdown and not deadlock on teardown.
      while (dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC)) != 0) {
        std::shared_ptr<img_t> probe_img;
        if (!pull_free_image_cb(probe_img)) {
          // Session is shutting down. Signal the semaphore so we exit the wait.
          // Don't call stopCapture here — the callback will detect shutdown on its
          // next invocation and do a clean teardown. Force-stopping AVFoundation
          // corrupts capture state for this display, causing black frames on reconnect.
          dispatch_semaphore_signal(signal);
          break;
        }
      }
      return capture_e::ok;
    }

    /**
     * @brief Allocate an image buffer compatible with this display backend.
     *
     * @return Allocated img object, or null when unavailable.
     */
    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    /**
     * @brief Create AVCodec encode device.
     *
     * @param pix_fmt Sunshine pixel format to convert or allocate for.
     * @return Constructed AVCodec encode device object.
     */
    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        av_capture.pixelFormat = kCVPixelFormatType_32BGRA;
        return std::make_unique<avcodec_encode_device_t>();
      } else if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();
        device->init(static_cast<void *>(av_capture), pix_fmt, setResolution, setPixelFormat);
        return device;
      } else {
        BOOST_LOG(error) << "Unsupported Pixel Format."sv;
        return nullptr;
      }
    }

    /**
     * @brief Populate a fallback image when real capture data is unavailable.
     *
     * @param img Image or frame object to read from or populate.
     * @return Capture status reported to the streaming pipeline.
     */
    int dummy_img(img_t *img) override {
      if (!platf::is_screen_capture_allowed()) {
        return 1;
      }

      // Create a synthetic dummy frame to avoid starting a capture session.
      // Starting and stopping AVFoundation capture for dummy_img can cause
      // timeouts on virtual displays and interfere with the main capture.
      int w = av_capture.frameWidth;
      int h = av_capture.frameHeight;

      CVPixelBufferRef pixelBuffer = NULL;
      NSDictionary *attrs = @{
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
      };
      CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault, w, h,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attrs,
        &pixelBuffer);

      if (status != kCVReturnSuccess || !pixelBuffer) {
        BOOST_LOG(error) << "AVFoundation dummy_img: failed to create pixel buffer"sv;
        return 1;
      }

      CVPixelBufferLockBaseAddress(pixelBuffer, 0);
      void *base = CVPixelBufferGetBaseAddress(pixelBuffer);
      memset(base, 0, CVPixelBufferGetBytesPerRow(pixelBuffer) * h);
      CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

      auto av_img = (av_img_t *)img;
      CMVideoFormatDescriptionRef formatDesc = NULL;
      CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &formatDesc);

      CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
      CMSampleBufferRef sampleBuffer = NULL;
      CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, YES, NULL, NULL, formatDesc, &timing, &sampleBuffer);

      if (formatDesc) CFRelease(formatDesc);

      if (sampleBuffer) {
        auto new_sample = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel = std::make_shared<av_pixel_buf_t>(new_sample->buf);
        av_img->sample_buffer = new_sample;
        av_img->pixel_buffer = new_pixel;
        img->data = new_pixel->data();
        CFRelease(sampleBuffer);
      }

      CVPixelBufferRelease(pixelBuffer);

      img->width = w;
      img->height = h;
      img->row_pitch = w * 4;
      img->pixel_pitch = 4;
      return 0;
    }

    /**
     * A bridge from the pure C++ code of the hwdevice_t class to the pure Objective C code.
     *
     * display --> an opaque pointer to an object of this class
     * width --> the intended capture width
     * height --> the intended capture height
     * @param display Display object or identifier associated with the operation.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     */
    static void setResolution(void *display, int width, int height) {
      [static_cast<AVVideo *>(display) setFrameWidth:width frameHeight:height];
    }

    /**
     * @brief Set pixel format.
     *
     * @param display Display object or identifier associated with the operation.
     * @param pixelFormat Pixel format.
     */
    static void setPixelFormat(void *display, OSType pixelFormat) {
      static_cast<AVVideo *>(display).pixelFormat = pixelFormat;
    }
  };

  // ── ScreenCaptureKit capture backend (macOS 12.3+) ──────────────────

  struct sc_display_t: public display_t {
    SCCapture *sc_capture {};
    CGDirectDisplayID display_id {};
    std::unique_ptr<display_device::DisplayPowerGuardInterface> display_power_guard;

    ~sc_display_t() override {
      [sc_capture stopCapture];
      [sc_capture release];
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto signal = [sc_capture captureVideo:^(CMSampleBufferRef sampleBuffer) {
        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return false;
        }
        if (!process_frame(sampleBuffer, img_out.get())) {
          return true;  // skip this frame but continue capturing
        }
        if (!push_captured_image_cb(std::move(img_out), true)) {
          return false;
        }
        return true;
      } audioCallback:nil];

      if (!signal) {
        BOOST_LOG(error) << "SCCapture failed to start video capture"sv;
        return capture_e::capture_error;
      }

      // Poll with timeout instead of waiting forever, so the capture thread
      // can respond to session shutdown and not deadlock on teardown.
      while (dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC)) != 0) {
        // Check if session is ending — pull_free_image_cb returns false when
        // the session is shutting down (capture_ctx_queue->running() == false).
        // This is needed because SCCapture may not deliver frames on an idle
        // display, so the video callback never gets a chance to detect shutdown.
        std::shared_ptr<img_t> probe_img;
        if (!pull_free_image_cb(probe_img)) {
          [sc_capture stopCapture];
          break;
        }
      }
      return capture_e::ok;
    }

    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        sc_capture.pixelFormat = kCVPixelFormatType_32BGRA;
        return std::make_unique<avcodec_encode_device_t>();
      } else if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();
        device->init(static_cast<void *>(sc_capture), pix_fmt, setResolution, setPixelFormat);
        return device;
      } else {
        BOOST_LOG(error) << "Unsupported Pixel Format."sv;
        return nullptr;
      }
    }

    int dummy_img(img_t *img) override {
      if (!platf::is_screen_capture_allowed()) {
        return 1;
      }

      // Create a synthetic dummy frame instead of starting a separate capture.
      // Starting and stopping a capture just for dummy_img causes SCK to
      // not deliver frames to the subsequent main capture stream.
      int w = sc_capture.frameWidth;
      int h = sc_capture.frameHeight;

      CVPixelBufferRef pixelBuffer = NULL;
      NSDictionary *attrs = @{
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
      };
      CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault, w, h,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attrs,
        &pixelBuffer);

      if (status != kCVReturnSuccess || !pixelBuffer) {
        BOOST_LOG(error) << "SCCapture dummy_img: failed to create pixel buffer"sv;
        return 1;
      }

      // Fill with black
      CVPixelBufferLockBaseAddress(pixelBuffer, 0);
      void *base = CVPixelBufferGetBaseAddress(pixelBuffer);
      memset(base, 0, CVPixelBufferGetBytesPerRow(pixelBuffer) * h);
      CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

      auto av_img = (av_img_t *)img;
      // Wrap in a CMSampleBuffer for the av_img pipeline
      CMVideoFormatDescriptionRef formatDesc = NULL;
      CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &formatDesc);

      CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
      CMSampleBufferRef sampleBuffer = NULL;
      CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, YES, NULL, NULL, formatDesc, &timing, &sampleBuffer);

      if (formatDesc) CFRelease(formatDesc);

      if (sampleBuffer) {
        auto new_sample = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel = std::make_shared<av_pixel_buf_t>(new_sample->buf);
        av_img->sample_buffer = new_sample;
        av_img->pixel_buffer = new_pixel;
        img->data = new_pixel->data();
        CFRelease(sampleBuffer);
      }

      CVPixelBufferRelease(pixelBuffer);

      img->width = w;
      img->height = h;
      img->row_pitch = w * 4;
      img->pixel_pitch = 4;
      return 0;
    }

    static void setResolution(void *display, int width, int height) {
      [static_cast<SCCapture *>(display) setFrameWidth:width frameHeight:height];
    }

    static void setPixelFormat(void *display, OSType pixelFormat) {
      static_cast<SCCapture *>(display).pixelFormat = pixelFormat;
    }
  };

  // ── Display factory ─────────────────────────────────────────────────

  std::shared_ptr<display_t> display(platf::mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    BOOST_LOG(debug) << "Waking display for capture selector ["sv << display_name << ']';
    if (!display_device::wake_display(display_name, 1s)) {
      BOOST_LOG(debug) << "Display wake attempt did not expose the requested display ["sv << display_name << ']';
    }

    auto display_power_guard = display_device::keep_display_awake("Sunshine display capture");
    if (display_power_guard) {
      BOOST_LOG(debug) << "Keeping display awake for capture"sv;
    } else {
      BOOST_LOG(debug) << "Unable to create display sleep prevention assertion"sv;
    }

    // Default to main display; an active virtual display takes precedence over the configured one
    CGDirectDisplayID display_id = CGMainDisplayID();

    if (const auto virtual_display_id = platf::virtual_display_get_id(); virtual_display_id != 0) {
      BOOST_LOG(info) << "Using virtual display (id: "sv << virtual_display_id << ") for capture"sv;
      display_id = virtual_display_id;
    } else if (const auto configured_display_id {parse_display_id(display_name)}) {
      display_id = *configured_display_id;
    } else if (!display_name.empty()) {
      BOOST_LOG(warning) << "Configured display ["sv << display_name
                         << "] is not a valid macOS capture display id. Falling back to main display ["sv
                         << display_id << "]."sv;
    }

    // Print all displays available with their names and ids
    BOOST_LOG(debug) << "Detecting displays"sv;
    for (const auto &device : display_device::enumerate_devices()) {
      if (device.m_display_name.empty()) {
        continue;
      }

      BOOST_LOG(debug) << "Detected display: "sv << device.m_friendly_name
                       << " (id: "sv << device.m_display_name << ") connected: true"sv;
    }

    BOOST_LOG(info) << "Configuring selected display ("sv << display_id << ") to stream"sv;

    std::shared_ptr<display_t> display;

    // ScreenCaptureKit first: AVCaptureScreenInput stops delivering frames
    // when a virtual display is destroyed and recreated between sessions.
    if (@available(macOS 12.3, *)) {
      if ([SCCapture isAvailable]) {
        if (SCCapture *capture = [[SCCapture alloc] initWithDisplay:display_id frameRate:config.framerate captureAudio:NO]; capture != nil) {
          auto sc_display = std::make_shared<sc_display_t>();
          sc_display->display_id = display_id;
          sc_display->sc_capture = capture;
          sc_display->display_power_guard = std::move(display_power_guard);
          sc_display->width = capture.frameWidth;
          sc_display->height = capture.frameHeight;

          if (hwdevice_type == platf::mem_type_e::videotoolbox) {
            [capture setFrameWidth:config.width frameHeight:config.height];
            capture.pixelFormat = videotoolbox_pixel_format(config);
          }
          display = sc_display;
        } else {
          BOOST_LOG(error) << "SCCapture setup failed, trying AVFoundation..."sv;
        }
      }
    }

    if (!display) {
      AVVideo *capture = [[AVVideo alloc] initWithDisplay:display_id frameRate:config.framerate];
      if (!capture) {
        BOOST_LOG(error) << "Video setup failed."sv;
        return nullptr;
      }

      auto av_display = std::make_shared<av_display_t>();
      av_display->display_id = display_id;
      av_display->av_capture = capture;
      av_display->display_power_guard = std::move(display_power_guard);
      av_display->width = capture.frameWidth;
      av_display->height = capture.frameHeight;

      if (hwdevice_type == platf::mem_type_e::videotoolbox) {
        [capture setFrameWidth:config.width frameHeight:config.height];
        capture.pixelFormat = videotoolbox_pixel_format(config);
      }
      display = av_display;
    }

    const auto selected_display_bounds = CGDisplayBounds(display_id);
    const auto desktop_bounds = active_desktop_bounds(display_id);
    display->offset_x = static_cast<int>(selected_display_bounds.origin.x);
    display->offset_y = static_cast<int>(selected_display_bounds.origin.y);
    display->logical_width = static_cast<int>(selected_display_bounds.size.width);
    display->logical_height = static_cast<int>(selected_display_bounds.size.height);
    display->env_offset_x = static_cast<int>(desktop_bounds.origin.x);
    display->env_offset_y = static_cast<int>(desktop_bounds.origin.y);
    display->env_logical_width = static_cast<int>(desktop_bounds.size.width);
    display->env_logical_height = static_cast<int>(desktop_bounds.size.height);
    display->env_width = display->env_logical_width;
    display->env_height = display->env_logical_height;

    BOOST_LOG(debug) << "Selected display bounds: "sv << display->offset_x << 'x' << display->offset_y << ' '
                     << display->logical_width << 'x' << display->logical_height;
    BOOST_LOG(debug) << "Active desktop bounds: "sv << display->env_offset_x << 'x' << display->env_offset_y << ' '
                     << display->env_logical_width << 'x' << display->env_logical_height;

    return display;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    std::vector<std::string> display_names;
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      return display_names;
    }

    const auto devices {display_device::enumerate_devices()};
    display_names.reserve(devices.size());
    for (const auto &device : devices) {
      if (!device.m_display_name.empty()) {
        display_names.emplace_back(device.m_display_name);
      }
    }

    return display_names;
  }

  /**
   * @brief Report whether encoder backends should be probed again before streaming.
   *
   * @return Always `true` because macOS GPU changes are not tracked by this backend.
   */
  bool needs_encoder_reenumeration() {
    // We don't track GPU state, so we will always reenumerate. Fortunately, it is fast on macOS.
    return true;
  }
}  // namespace platf
