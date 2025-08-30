/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GHOST_SystemIOS.hh"

#include "GHOST_ContextIOS.hh"
#include "GHOST_WindowIOS.hh"

#include "GHOST_Debug.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventDragnDrop.hh"
#include "GHOST_EventString.hh"
#include "GHOST_WindowManager.hh"

#ifdef WITH_INPUT_NDOF
#  include "GHOST_NDOFManagerCocoa.hh"
#endif

#import <MetalKit/MTKView.h>
#import <UIKit/UIKit.h>

#include <sys/sysctl.h>
#include <sys/time.h>

// #define IOS_SYSTEM_LOGGING
#if defined(IOS_SYSTEM_LOGGING)
#  define IOS_SYSTEM_LOG(...) NSLog(__VA_ARGS__)
#else
#  define IOS_SYSTEM_LOG(...)
#endif

extern "C" {
struct bContext;
static bContext *C = nullptr;
}

int argc = 0;
const char **argv = nullptr;

/* Implemented in wm.cc. */
void WM_main_loop_body(bContext *C);
int main_ios_callback(int argc, const char **argv);

@interface IOSAppDelegate : UIResponder <UIApplicationDelegate>

@property(strong, nonatomic) UIWindow *window;

@end

@implementation IOSAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
  main_ios_callback(argc, argv);

  return YES;
}

- (BOOL)application:(UIApplication *)application
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options
{
  GHOST_SystemIOS *system = static_cast<GHOST_SystemIOS *>(GHOST_ISystem::getSystem());

  system->handleOpenDocumentRequest(url.path);

  return YES;
}

@end

@implementation GHOST_IOSMetalRenderer
{
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
}

- (nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)mtkView
{
  self = [super init];
  if (self) {
    _device = mtkView.device;

    /* Create the command queue. */
    _commandQueue = [_device newCommandQueue];
  }

  return self;
}

- (void)drawInMTKView:(nonnull MTKView *)MTKView
{
  GHOST_SystemIOS *system = static_cast<GHOST_SystemIOS *>(GHOST_ISystem::getSystem());

  /* We should always have a window... */
  if (system->current_active_window_) {

    /* If the current window has some outstanding swaps we need to
     * service them before handing control back to Blender otherwise
     * they may go missing. */
    if (system->current_active_window_->deferred_swap_buffers_count) {
      IOS_SYSTEM_LOG(@"Issuing oustanding swaps");
      system->current_active_window_->flushDeferredSwapBuffers();
      /* Make sure we get another call to draw. */
      system->current_active_window_->needsDisplayUpdate();
      return;
    }

    system->current_active_window_->beginFrame();
  }

  /* Run the main loop to handle all events. */
  if (C) {
    WM_main_loop_body(C);
  }

  if (system->current_active_window_) {
    system->current_active_window_->flushDeferredSwapBuffers();
    system->current_active_window_->endFrame();
  }

  /* Was there a request to switch windows? */
  if (system->next_active_window_ != nullptr) {
    if (system->current_active_window_) {
      system->current_active_window_->resignKeyWindow();
    }
    system->next_active_window_->makeKeyWindow();
    system->next_active_window_ = nullptr;
  }
}

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size
{
  GHOST_SystemIOS *system = static_cast<GHOST_SystemIOS *>(GHOST_ISystem::getSystem());
  if (!system->current_active_window_) {
    return;
  }

  system->pushEvent(new GHOST_Event(
      system->getMilliSeconds(), GHOST_kEventWindowSize, system->current_active_window_));
}

@end

int GHOST_iosmain(int _argc, const char **_argv)
{
  argc = _argc;
  argv = _argv;
  @autoreleasepool {
    return UIApplicationMain(
        _argc, (char *_Nullable *)_argv, nil, NSStringFromClass([IOSAppDelegate class]));
  }
}

void GHOST_iosfinalize(bContext *CTX)
{
  C = CTX;
}

#pragma mark KeyMap, mouse converters

GHOST_TButton convertButton(int button)
{
  switch (button) {
    case 0:
      return GHOST_kButtonMaskLeft;
    case 2:
      return GHOST_kButtonMaskRight;
    case 4:
      return GHOST_kButtonMaskMiddle;
    case 8:
      return GHOST_kButtonMaskButton4;
    case 16:
      return GHOST_kButtonMaskButton5;
    case 32:
      return GHOST_kButtonMaskButton6;
    case 64:
      return GHOST_kButtonMaskButton7;
    default:
      return GHOST_kButtonMaskLeft;
  }
}

GHOST_TKey convertIOSKeyToGHOST(NSString *key)
{
  /* Handle special keys using string comparison (iOS 7.0+) */
  if (@available(iOS 7.0, *)) {
    if ([key isEqualToString:UIKeyInputEscape]) {
      return GHOST_kKeyEsc;
    }
    if ([key isEqualToString:UIKeyInputUpArrow]) {
      return GHOST_kKeyUpArrow;
    }
    if ([key isEqualToString:UIKeyInputDownArrow]) {
      return GHOST_kKeyDownArrow;
    }
    if ([key isEqualToString:UIKeyInputLeftArrow]) {
      return GHOST_kKeyLeftArrow;
    }
    if ([key isEqualToString:UIKeyInputRightArrow]) {
      return GHOST_kKeyRightArrow;
    }
    if ([key isEqualToString:UIKeyInputDelete]) {
      return GHOST_kKeyBackSpace;
    }
  }

  /* Handle additional special keys by string name */
  if ([key isEqualToString:UIKeyInputF1]) {
    return GHOST_kKeyF1;
  }
  if ([key isEqualToString:UIKeyInputF2]) {
    return GHOST_kKeyF2;
  }
  if ([key isEqualToString:UIKeyInputF3]) {
    return GHOST_kKeyF3;
  }
  if ([key isEqualToString:UIKeyInputF4]) {
    return GHOST_kKeyF4;
  }
  if ([key isEqualToString:UIKeyInputF5]) {
    return GHOST_kKeyF5;
  }
  if ([key isEqualToString:UIKeyInputF6]) {
    return GHOST_kKeyF6;
  }
  if ([key isEqualToString:UIKeyInputF7]) {
    return GHOST_kKeyF7;
  }
  if ([key isEqualToString:UIKeyInputF8]) {
    return GHOST_kKeyF8;
  }
  if ([key isEqualToString:UIKeyInputF9]) {
    return GHOST_kKeyF9;
  }
  if ([key isEqualToString:UIKeyInputF10]) {
    return GHOST_kKeyF10;
  }
  if ([key isEqualToString:UIKeyInputF11]) {
    return GHOST_kKeyF11;
  }
  if ([key isEqualToString:UIKeyInputF12]) {
    return GHOST_kKeyF12;
  }

  /* Additional navigation and editing keys */
  if ([key isEqualToString:@"Home"]) {
    return GHOST_kKeyHome;
  }
  if ([key isEqualToString:@"End"]) {
    return GHOST_kKeyEnd;
  }
  if ([key isEqualToString:@"Page Up"]) {
    return GHOST_kKeyUpPage;
  }
  if ([key isEqualToString:@"Page Down"]) {
    return GHOST_kKeyDownPage;
  }
  if ([key isEqualToString:@"Insert"]) {
    return GHOST_kKeyInsert;
  }
  if ([key isEqualToString:@"Delete"]) {
    return GHOST_kKeyDelete;
  }

  /* Keypad/Numeric keys */
  if ([key isEqualToString:@"Keypad 0"]) {
    return GHOST_kKeyNumpad0;
  }
  if ([key isEqualToString:@"Keypad 1"]) {
    return GHOST_kKeyNumpad1;
  }
  if ([key isEqualToString:@"Keypad 2"]) {
    return GHOST_kKeyNumpad2;
  }
  if ([key isEqualToString:@"Keypad 3"]) {
    return GHOST_kKeyNumpad3;
  }
  if ([key isEqualToString:@"Keypad 4"]) {
    return GHOST_kKeyNumpad4;
  }
  if ([key isEqualToString:@"Keypad 5"]) {
    return GHOST_kKeyNumpad5;
  }
  if ([key isEqualToString:@"Keypad 6"]) {
    return GHOST_kKeyNumpad6;
  }
  if ([key isEqualToString:@"Keypad 7"]) {
    return GHOST_kKeyNumpad7;
  }
  if ([key isEqualToString:@"Keypad 8"]) {
    return GHOST_kKeyNumpad8;
  }
  if ([key isEqualToString:@"Keypad 9"]) {
    return GHOST_kKeyNumpad9;
  }
  if ([key isEqualToString:@"Keypad ."]) {
    return GHOST_kKeyNumpadPeriod;
  }
  if ([key isEqualToString:@"Keypad +"]) {
    return GHOST_kKeyNumpadPlus;
  }
  if ([key isEqualToString:@"Keypad -"]) {
    return GHOST_kKeyNumpadMinus;
  }
  if ([key isEqualToString:@"Keypad *"]) {
    return GHOST_kKeyNumpadAsterisk;
  }
  if ([key isEqualToString:@"Keypad /"]) {
    return GHOST_kKeyNumpadSlash;
  }
  if ([key isEqualToString:@"Keypad Enter"]) {
    return GHOST_kKeyNumpadEnter;
  }

  /* For regular character keys, get the first character */
  unichar character = [key characterAtIndex:0];

  /* Handle common control characters */
  switch (character) {
    case '\r':
    case '\n':
      return GHOST_kKeyEnter;
    case '\t':
      return GHOST_kKeyTab;
    case ' ':
      return GHOST_kKeySpace;
    case 0x1B:
      return GHOST_kKeyEsc; /* ESC character */
    default:
      break;
  }

  /* Handle alphanumeric keys - convert to uppercase for consistency */
  if (character >= 'a' && character <= 'z') {
    return (GHOST_TKey)(GHOST_kKeyA + (character - 'a'));
  }
  if (character >= 'A' && character <= 'Z') {
    return (GHOST_TKey)(GHOST_kKeyA + (character - 'A'));
  }
  if (character >= '0' && character <= '9') {
    return (GHOST_TKey)(GHOST_kKey0 + (character - '0'));
  }

  /* Handle other special characters */
  switch (character) {
    case '-':
      return GHOST_kKeyMinus;
    case '=':
      return GHOST_kKeyEqual;
    case '[':
      return GHOST_kKeyLeftBracket;
    case ']':
      return GHOST_kKeyRightBracket;
    case '\\':
      return GHOST_kKeyBackslash;
    case ';':
      return GHOST_kKeySemicolon;
    case '\'':
      return GHOST_kKeyQuote;
    case '`':
      return GHOST_kKeyAccentGrave;
    case ',':
      return GHOST_kKeyComma;
    case '.':
      return GHOST_kKeyPeriod;
    case '/':
      return GHOST_kKeySlash;

    /* Shifted special characters */
    case '_':
      return GHOST_kKeyMinus; /* Shifted minus */
    case '+':
      return GHOST_kKeyEqual; /* Shifted equal */
    case '{':
      return GHOST_kKeyLeftBracket; /* Shifted [ */
    case '}':
      return GHOST_kKeyRightBracket; /* Shifted ] */
    case '|':
      return GHOST_kKeyBackslash; /* Shifted \ */
    case ':':
      return GHOST_kKeySemicolon; /* Shifted ; */
    case '"':
      return GHOST_kKeyQuote; /* Shifted ' */
    case '~':
      return GHOST_kKeyAccentGrave; /* Shifted ` */
    case '<':
      return GHOST_kKeyComma; /* Shifted , */
    case '>':
      return GHOST_kKeyPeriod; /* Shifted . */
    case '?':
      return GHOST_kKeySlash; /* Shifted / */

    /* Shifted number row */
    case '!':
      return GHOST_kKey1;
    case '@':
      return GHOST_kKey2;
    case '#':
      return GHOST_kKey3;
    case '$':
      return GHOST_kKey4;
    case '%':
      return GHOST_kKey5;
    case '^':
      return GHOST_kKey6;
    case '&':
      return GHOST_kKey7;
    case '*':
      return GHOST_kKey8;
    case '(':
      return GHOST_kKey9;
    case ')':
      return GHOST_kKey0;

    /* Additional control characters */
    case 0x08:
      return GHOST_kKeyBackSpace; /* Backspace */
    case 0x7F:
      return GHOST_kKeyDelete; /* Delete */

    default:
      return GHOST_kKeyUnknown;
  }
}

GHOST_TKey convertIOSModToGHOST(int keyCode)
{
  switch (keyCode) {
    case 225: /* Left Shift */
      return GHOST_kKeyLeftShift;
    case 229: /* Right Shift */
      return GHOST_kKeyRightShift;
    case 224: /* Left Control */
      return GHOST_kKeyLeftControl;
    case 228: /* Right Control */
      return GHOST_kKeyRightControl;
    case 226: /* Left Alt/Option */
      return GHOST_kKeyLeftAlt;
    case 230: /* Right Alt/Option */
      return GHOST_kKeyRightAlt;
    case 227: /* Left Command (⌘) */
      return GHOST_kKeyLeftOS;
    case 231: /* Right Command (⌘) */
      return GHOST_kKeyRightOS;
    default:
      return GHOST_kKeyUnknown;
  }
}

#pragma mark Utility functions

#define FIRSTFILEBUFLG 512
static bool g_hasFirstFile = false;
static char g_firstFileBuf[512];

extern "C" int GHOST_HACK_getFirstFile(char buf[FIRSTFILEBUFLG])
{
  if (g_hasFirstFile) {
    strncpy(buf, g_firstFileBuf, FIRSTFILEBUFLG - 1);
    buf[FIRSTFILEBUFLG - 1] = '\0';
    return 1;
  }
  else {
    return 0;
  }
}

#pragma mark initialization/finalization

GHOST_SystemIOS::GHOST_SystemIOS()
{
  int mib[2];
  struct timeval boottime;
  size_t len;
  char *rstring = NULL;

  modifier_mask_ = 0;
  outside_loop_event_processed_ = false;
  need_delayed_application_become_active_event_processing_ = false;

  /* TODO: sysctl likely should be replaced with another approach. */
  mib[0] = CTL_KERN;
  mib[1] = KERN_BOOTTIME;
  len = sizeof(struct timeval);

  sysctl(mib, 2, &boottime, &len, NULL, 0);
  m_start_time = ((boottime.tv_sec * 1000) + (boottime.tv_usec / 1000));

  /* Detect multi-touch track-pad. */
  mib[0] = CTL_HW;
  mib[1] = HW_MODEL;
  sysctl(mib, 2, NULL, &len, NULL, 0);
  rstring = (char *)malloc(len);
  sysctl(mib, 2, rstring, &len, NULL, 0);

  free(rstring);
  rstring = NULL;

  ignore_window_sized_message_ = false;
  ignore_momentum_scroll_ = false;
  multi_touch_scroll_ = false;
  last_warp_timestamp_ = 0;
}

GHOST_SystemIOS::~GHOST_SystemIOS() {}

GHOST_TSuccess GHOST_SystemIOS::init()
{
  GHOST_TSuccess success = GHOST_System::init();
  if (success) {

#ifdef WITH_INPUT_NDOF
    m_ndofManager = new GHOST_NDOFManagerCocoa(*this);
#endif
  }
  return success;
}

#pragma mark window management

uint64_t GHOST_SystemIOS::getMilliSeconds() const
{
  struct timeval currentTime;

  gettimeofday(&currentTime, NULL);
  return ((currentTime.tv_sec * 1000) + (currentTime.tv_usec / 1000) - m_start_time);
}

uint8_t GHOST_SystemIOS::getNumDisplays() const
{
  return 1;
}

void GHOST_SystemIOS::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  CGRect screenRect = [[UIScreen mainScreen] bounds];
  CGFloat scaling_fac = [UIScreen mainScreen].scale;
  CGFloat screenWidth = screenRect.size.width * scaling_fac;
  CGFloat screenHeight = screenRect.size.height * scaling_fac;

  if (screenWidth <= 0 || screenHeight <= 0) {
    GHOST_ASSERT(false, "Negative or null display dimmensions");
    screenWidth = 2532;
    screenHeight = 1170;
  }

  width = screenWidth;
  height = screenHeight;
}

void GHOST_SystemIOS::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  /* TOOD: iOS passthrough. */
  getMainDisplayDimensions(width, height);
}

GHOST_IWindow *GHOST_SystemIOS::createWindow(const char *title,
                                             int32_t /*left*/,
                                             int32_t /*top*/,
                                             uint32_t /*width*/,
                                             uint32_t /*height*/,
                                             GHOST_TWindowState state,
                                             GHOST_GPUSettings gpu_settings,
                                             const bool /*exclusive*/,
                                             const bool is_dialog,
                                             const GHOST_IWindow *parent_window)
{
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(gpu_settings);
  GHOST_IWindow *window = nullptr;
  @autoreleasepool {

    /* Create window at native size. */
    CGRect bounds = [[UIScreen mainScreen] bounds];

    window = (GHOST_IWindow *)new GHOST_WindowIOS(this,
                                                  title,
                                                  (int)bounds.origin.x,
                                                  (int)bounds.origin.y,
                                                  (unsigned int)bounds.size.width,
                                                  (unsigned int)bounds.size.height,
                                                  state,
                                                  gpu_settings.context_type,
                                                  context_params,
                                                  is_dialog,
                                                  (GHOST_WindowIOS *)parent_window);

    if (window->getValid()) {
      // Store the pointer to the window
      GHOST_ASSERT(window_manager_, "m_windowManager not initialized");
      window_manager_->addWindow(window);
      window_manager_->setActiveWindow(window);
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowActivate, window));
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window));
    }
    else {
      GHOST_PRINT("GHOST_SystemIOS::createWindow(): window invalid\n");
      delete window;
      window = nullptr;
    }
  }
  return window;
}

/**
 * Create a new offscreen context.
 * Never explicitly delete the context, use #disposeContext() instead.
 * \return The new context (or 0 if creation failed).
 */
GHOST_IContext *GHOST_SystemIOS::createOffscreenContext(GHOST_GPUSettings gpu_settings)
{
  const GHOST_ContextParams context_params_offscreen =
      GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(gpu_settings);

  GHOST_Context *context = new GHOST_ContextIOS(context_params_offscreen, nullptr, nullptr);
  if (context->initializeDrawingContext()) {
    return context;
  }

  delete context;
  return nullptr;
}

/**
 * Dispose of a context.
 * \param context: Pointer to the context to be disposed.
 * \return Indication of success.
 */
GHOST_TSuccess GHOST_SystemIOS::disposeContext(GHOST_IContext *context)
{
  delete context;

  return GHOST_kSuccess;
}

/**
 * \note : returns 0,0 on ios as no cursor is present.
 * TODO: If external mouse or trackpad is connected, we can query cursor position.
 */
GHOST_TSuccess GHOST_SystemIOS::getCursorPosition(int32_t & /*x*/, int32_t & /*y*/) const
{
  /* iOS Passthrough. */
  GHOST_IWindow *window = this->window_manager_->getActiveWindow();
  if (!window) {
    return GHOST_kFailure;
  }
  // GHOST_ASSERT(FALSE,"GHOST_SystemIOS::getCursorPosition unsupported on iOS");
  return GHOST_kSuccess;
}

/**
 * \note : expect Cocoa screen coordinates
 * TODO: If external mouse or trackpad is connected, we can set cursor position.
 */
GHOST_TSuccess GHOST_SystemIOS::setCursorPosition(int32_t x, int32_t y)
{
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;

  pushEvent(new GHOST_EventCursor(
      getMilliSeconds(), GHOST_kEventCursorMove, window, x, y, window->getTabletData()));
  outside_loop_event_processed_ = true;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::setMouseCursorPosition(int32_t /*x*/, int32_t /*y*/)
{
  /* iOS Passthrough. */
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;
  GHOST_ASSERT(FALSE, "GHOST_SystemIOS::setMouseCursorPosition unsupported on iOS");
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::getModifierKeys(GHOST_ModifierKeys & /*keys*/) const
{
  /* iOS Passthrough. */
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::getButtons(GHOST_Buttons & /*buttons*/) const
{
  /* iOS Passthrough. */
  return GHOST_kSuccess;
}
GHOST_TCapabilityFlag GHOST_SystemIOS::getCapabilities() const
{
  return GHOST_TCapabilityFlag(GHOST_kCapabilityGPUReadFrontBuffer);
}

#pragma mark Event handlers

/**
 * The event queue polling function
 */
bool GHOST_SystemIOS::processEvents(bool /*waitForEvent*/)
{
  /*
   Touch screen events are being processed through the UIView interactions
   We may need some additional code here to handle key presses if an external keybaord
   is attached
   */
  return true;
}

GHOST_TSuccess GHOST_SystemIOS::handleApplicationBecomeActiveEvent()
{
  modifier_mask_ = 0;

  outside_loop_event_processed_ = true;
  return GHOST_kSuccess;
}

bool GHOST_SystemIOS::hasDialogWindow()
{
  for (GHOST_IWindow *iwindow : window_manager_->getWindows()) {
    GHOST_WindowIOS *window = (GHOST_WindowIOS *)iwindow;
    if (window->isDialog()) {
      return true;
    }
  }
  return false;
}

void GHOST_SystemIOS::notifyExternalEventProcessed()
{
  outside_loop_event_processed_ = true;
}

GHOST_TSuccess GHOST_SystemIOS::handleWindowEvent(GHOST_TEventType eventType,
                                                  GHOST_WindowIOS *window)
{
  if (!validWindow(window)) {
    return GHOST_kFailure;
  }
  switch (eventType) {
    case GHOST_kEventWindowClose:
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowClose, window));
      break;
    case GHOST_kEventWindowActivate:
      window_manager_->setActiveWindow(window);
      window->loadCursor(window->getCursorVisibility(), window->getCursorShape());
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowActivate, window));
      break;
    case GHOST_kEventWindowDeactivate:
      window_manager_->setWindowInactive(window);
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowDeactivate, window));
      break;
    case GHOST_kEventWindowUpdate:
      if (native_pixel_) {
        window->setNativePixelSize();
        pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventNativeResolutionChange, window));
      }
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowUpdate, window));
      break;
    case GHOST_kEventWindowMove:
      pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowMove, window));
      break;
    case GHOST_kEventWindowSize:
      if (!ignore_window_sized_message_) {
        // Enforce only one resize message per event loop
        // (coalescing all the live resize messages)
        window->updateDrawingContext();
        pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window));
        // Mouse up event is trapped by the resizing event loop,
        // so send it anyway to the window manager.
        pushEvent(new GHOST_EventButton(getMilliSeconds(),
                                        GHOST_kEventButtonUp,
                                        window,
                                        GHOST_kButtonMaskLeft,
                                        GHOST_TABLET_DATA_NONE));
      }
      break;
    case GHOST_kEventNativeResolutionChange:

      if (native_pixel_) {
        pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventNativeResolutionChange, window));
      }

    default:
      return GHOST_kFailure;
      break;
  }

  outside_loop_event_processed_ = true;

  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemIOS::popupOnScreenKeyboard(
    GHOST_IWindow *window, const GHOST_KeyboardProperties &keyboard_properties)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }
  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;
  return windowIOS->popupOnscreenKeyboard(keyboard_properties);
}

GHOST_TSuccess GHOST_SystemIOS::hideOnScreenKeyboard(GHOST_IWindow *window)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }

  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;

  return windowIOS->hideOnscreenKeyboard();
}

const char *GHOST_SystemIOS::getKeyboardInput(GHOST_IWindow *window)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return nullptr;
  }

  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;

  return windowIOS->getLastKeyboardString();
}

bool GHOST_SystemIOS::getExternalKeyboard(GHOST_IWindow *window)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return false;
  }

  GHOST_WindowIOS *windowIOS = (GHOST_WindowIOS *)window;

  return windowIOS->getExternalKeyboard();
}

GHOST_TSuccess GHOST_SystemIOS::startSecurityScopedFileAccess(const char *filepath)
{
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filepath]];
  BOOL success = [url startAccessingSecurityScopedResource];

  return success ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_SystemIOS::stopSecurityScopedFileAccess(const char *filepath)
{
  NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:filepath]];
  [url stopAccessingSecurityScopedResource];

  return GHOST_kSuccess;
}

// Note: called from NSWindow subclass
GHOST_TSuccess GHOST_SystemIOS::handleDraggingEvent(GHOST_TEventType eventType,
                                                    GHOST_TDragnDropTypes draggedObjectType,
                                                    GHOST_WindowIOS *window,
                                                    int mouseX,
                                                    int mouseY,
                                                    void *data)
{
  if (!validWindow((GHOST_IWindow *)window)) {
    return GHOST_kFailure;
  }
  switch (eventType) {
    case GHOST_kEventDraggingEntered:
    case GHOST_kEventDraggingUpdated:
    case GHOST_kEventDraggingExited:
      window->clientToScreenIntern(mouseX, mouseY, mouseX, mouseY);
      pushEvent(new GHOST_EventDragnDrop(
          getMilliSeconds(), eventType, draggedObjectType, window, mouseX, mouseY, nullptr));
      break;

    case GHOST_kEventDraggingDropDone: {
      uint8_t *temp_buff;
      GHOST_TStringArray *strArray;
      NSArray *droppedArray;
      size_t pastedTextSize;
      NSString *droppedStr;
      GHOST_TDragnDropDataPtr eventData;
      int i;

      if (!data)
        return GHOST_kFailure;

      switch (draggedObjectType) {
        case GHOST_kDragnDropTypeFilenames:
          droppedArray = (NSArray *)data;

          strArray = (GHOST_TStringArray *)malloc(sizeof(GHOST_TStringArray));
          if (!strArray)
            return GHOST_kFailure;

          strArray->count = [droppedArray count];
          if (strArray->count == 0) {
            free(strArray);
            return GHOST_kFailure;
          }

          strArray->strings = (uint8_t **)malloc(strArray->count * sizeof(uint8_t *));

          for (i = 0; i < strArray->count; i++) {
            droppedStr = [droppedArray objectAtIndex:i];

            pastedTextSize = [droppedStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            temp_buff = (uint8_t *)malloc(pastedTextSize + 1);

            if (!temp_buff) {
              strArray->count = i;
              break;
            }

            strncpy((char *)temp_buff,
                    [droppedStr cStringUsingEncoding:NSUTF8StringEncoding],
                    pastedTextSize);
            temp_buff[pastedTextSize] = '\0';

            strArray->strings[i] = temp_buff;
          }

          eventData = static_cast<GHOST_TDragnDropDataPtr>(strArray);
          break;

        case GHOST_kDragnDropTypeString:
          droppedStr = (NSString *)data;
          pastedTextSize = [droppedStr lengthOfBytesUsingEncoding:NSUTF8StringEncoding];

          temp_buff = (uint8_t *)malloc(pastedTextSize + 1);

          if (temp_buff == NULL) {
            return GHOST_kFailure;
          }

          strncpy((char *)temp_buff,
                  [droppedStr cStringUsingEncoding:NSUTF8StringEncoding],
                  pastedTextSize);

          temp_buff[pastedTextSize] = '\0';

          eventData = static_cast<GHOST_TDragnDropDataPtr>(temp_buff);
          break;

        case GHOST_kDragnDropTypeBitmap: {
          /* Unsupported iOS. */
          return GHOST_kFailure;
          break;
        }
        default:
          return GHOST_kFailure;
          break;
      }

      pushEvent(new GHOST_EventDragnDrop(
          getMilliSeconds(), eventType, draggedObjectType, window, mouseX, mouseY, eventData));

      break;
    }
    default:
      return GHOST_kFailure;
  }
  outside_loop_event_processed_ = true;
  return GHOST_kSuccess;
}

void GHOST_SystemIOS::handleQuitRequest()
{
  GHOST_Window *window = (GHOST_Window *)window_manager_->getActiveWindow();

  // Discard quit event if we are in cursor grab sequence
  if (window && window->getCursorGrabModeIsWarp())
    return;

  // Push the event to Blender so it can open a dialog if needed
  pushEvent(new GHOST_Event(getMilliSeconds(), GHOST_kEventQuitRequest, window));
  outside_loop_event_processed_ = true;
}

bool GHOST_SystemIOS::handleOpenDocumentRequest(void *filepathStr)
{
  NSString *filepath = (NSString *)filepathStr;

  @autoreleasepool {
    if (!current_active_window_) {
      return NO;
    }

    /* Discard event if we are in cursor grab sequence,
     * it'll lead to "stuck cursor" situation if the alert panel is raised. */
    if (current_active_window_->getCursorGrabModeIsWarp()) {
      return NO;
    }

    const size_t filenameTextSize = [filepath lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    char *temp_buff = (char *)malloc(filenameTextSize + 1);

    if (temp_buff == nullptr) {
      return GHOST_kFailure;
    }

    memcpy(temp_buff, [filepath cStringUsingEncoding:NSUTF8StringEncoding], filenameTextSize);
    temp_buff[filenameTextSize] = '\0';

    pushEvent(new GHOST_EventString(getMilliSeconds(),
                                    GHOST_kEventOpenMainFile,
                                    current_active_window_,
                                    static_cast<GHOST_TEventDataPtr>(temp_buff)));
  }
  return YES;
}

/* None of this currently required for iOS */
#if 0
GHOST_TSuccess GHOST_SystemIOS::handleTabletEvent(void * /*eventPtr*/, short /*eventType*/)
{
  GHOST_WindowIOS *window = (GHOST_WindowIOS *)window_manager_->getActiveWindow();
  if (!window)
    return GHOST_kFailure;
  
  return GHOST_kSuccess;
}

bool GHOST_SystemIOS::handleTabletEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events. */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleTabletEvent unsupported on iOS");
  return true;
}

GHOST_TSuccess GHOST_SystemIOS::handleMouseEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events (here or elsewhere).
   * NOTE: "Touch" events already handled in other code paths above. */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleMouseEvent unsupported on iOS");
  return GHOST_kSuccess;
}

#  include <Metal/Metal.h>
bool frame_capture = false;
extern id<MTLDevice> extern_device;
GHOST_TSuccess GHOST_SystemIOS::handleKeyEvent(void * /*eventPtr*/)
{
  /* TODO: Handle events (here or elsewhere). */
  GHOST_ASSERT(FALSE,"GHOST_SystemIOS::handleKeyEvent unsupported on iOS");
  return GHOST_kSuccess;
}
#endif

#pragma mark Clipboard get/set

char *GHOST_SystemIOS::getClipboard(bool /*selection*/) const
{
  @autoreleasepool {
    UIPasteboard *pasteBoard = [UIPasteboard generalPasteboard];
    NSString *textPasted = pasteBoard.string;

    if (textPasted == nil) {
      return nullptr;
    }

    const size_t pastedTextSize = [textPasted lengthOfBytesUsingEncoding:NSUTF8StringEncoding];

    char *temp_buff = (char *)malloc(pastedTextSize + 1);

    if (temp_buff == nullptr) {
      return nullptr;
    }

    memcpy(temp_buff, [textPasted cStringUsingEncoding:NSUTF8StringEncoding], pastedTextSize);
    temp_buff[pastedTextSize] = '\0';
    return temp_buff;
  }
  return nullptr;
}

void GHOST_SystemIOS::putClipboard(const char *buffer, bool selection) const
{
  if (selection) {
    return; /* For copying the selection, used on X11. */
  }

  @autoreleasepool {
    UIPasteboard *pasteBoard = UIPasteboard.generalPasteboard;
    NSString *textToCopy = [NSString stringWithCString:buffer encoding:NSUTF8StringEncoding];
    [pasteBoard setString:textToCopy];
  }
}

GHOST_IWindow *GHOST_SystemIOS::getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/)
{
  GHOST_ASSERT(FALSE, "GHOST_SystemIOS::getWindowUnderCursor unsupported on iOS");
  return nullptr;
}
