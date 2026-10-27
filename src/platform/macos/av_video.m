/**
 * @file src/platform/macos/av_video.m
 * @brief Definitions for video capture on macOS.
 */
// local includes
#import "av_video.h"

@implementation AVVideo

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  self = [super init];

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
  if (!mode) {
    [self release];
    return nil;
  }

  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;
  if (mode) {
    self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
    self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
    CFRelease(mode);
  } else {
    // Virtual displays may not have a CGDisplayMode — use CGDisplay dimensions
    self.frameWidth = (int) CGDisplayPixelsWide(displayID);
    self.frameHeight = (int) CGDisplayPixelsHigh(displayID);
  }
  self.minFrameDuration = CMTimeMake(1, frameRate);
  self.session = [[AVCaptureSession alloc] init];
  self.videoOutputs = [[NSMapTable alloc] init];
  self.captureCallbacks = [[NSMapTable alloc] init];
  self.captureSignals = [[NSMapTable alloc] init];

  AVCaptureScreenInput *screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:self.displayID];
  if (!screenInput) {
    NSLog(@"[AVVideo] Failed to create AVCaptureScreenInput for display %u", self.displayID);
    return nil;
  }
  [screenInput setMinFrameDuration:self.minFrameDuration];
  [screenInput setCapturesMouseClicks:NO];

  if ([self.session canAddInput:screenInput]) {
    [self.session addInput:screenInput];
  } else {
    NSLog(@"[AVVideo] Cannot add screen input for display %u to session", self.displayID);
    [screenInput release];
    return nil;
  }

  [self.session startRunning];
  NSLog(@"[AVVideo] Capture session started for display %u (%dx%d)", self.displayID, self.frameWidth, self.frameHeight);

  return self;
}

- (void)dealloc {
  [self.videoOutputs release];
  [self.captureCallbacks release];
  [self.captureSignals release];
  [self.session stopRunning];
  [super dealloc];
}

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight {
  self.frameWidth = frameWidth;
  self.frameHeight = frameHeight;
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  @synchronized(self) {
    AVCaptureVideoDataOutput *videoOutput = [[AVCaptureVideoDataOutput alloc] init];

    [videoOutput setVideoSettings:@{
      (NSString *) kCVPixelBufferPixelFormatTypeKey: [NSNumber numberWithUnsignedInt:self.pixelFormat],
      (NSString *) kCVPixelBufferWidthKey: [NSNumber numberWithInt:self.frameWidth],
      (NSString *) kCVPixelBufferHeightKey: [NSNumber numberWithInt:self.frameHeight],
      (NSString *) AVVideoScalingModeKey: AVVideoScalingModeResizeAspect,
    }];

    dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_queue_t recordingQueue = dispatch_queue_create("videoCaptureQueue", qos);
    [videoOutput setSampleBufferDelegate:self queue:recordingQueue];

    [self.session stopRunning];

    if ([self.session canAddOutput:videoOutput]) {
      [self.session addOutput:videoOutput];
    } else {
      [videoOutput release];
      return nil;
    }

    AVCaptureConnection *videoConnection = [videoOutput connectionWithMediaType:AVMediaTypeVideo];
    dispatch_semaphore_t signal = dispatch_semaphore_create(0);

    [self.videoOutputs setObject:videoOutput forKey:videoConnection];
    [self.captureCallbacks setObject:frameCallback forKey:videoConnection];
    [self.captureSignals setObject:signal forKey:videoConnection];

    [self.session startRunning];

    return signal;
  }
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection *)connection {
  static int frameCount = 0;
  if (++frameCount <= 3 || frameCount % 300 == 0) {
    NSLog(@"[AVVideo] Frame %d received from display %u", frameCount, self.displayID);
  }
  FrameCallbackBlock callback = [self.captureCallbacks objectForKey:connection];

  if (callback != nil) {
    if (!callback(sampleBuffer)) {
      @synchronized(self) {
        [self.session stopRunning];
        [self.captureCallbacks removeObjectForKey:connection];
        [self.session removeOutput:[self.videoOutputs objectForKey:connection]];
        [self.videoOutputs removeObjectForKey:connection];
        dispatch_semaphore_signal([self.captureSignals objectForKey:connection]);
        [self.captureSignals removeObjectForKey:connection];
        [self.session startRunning];
      }
    }
  }
}

- (void)stopCapture {
  @synchronized(self) {
    [self.session stopRunning];
    // Signal all pending captures so waiting threads can exit
    for (AVCaptureConnection *conn in self.captureSignals.keyEnumerator.allObjects) {
      dispatch_semaphore_signal([self.captureSignals objectForKey:conn]);
    }
    [self.captureCallbacks removeAllObjects];
    [self.captureSignals removeAllObjects];
  }
}

@end
