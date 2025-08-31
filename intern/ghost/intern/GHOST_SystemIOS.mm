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

GHOST_TKey convertIOSKeyToGHOST(long key_value)
{
  UIKeyboardHIDUsage key = static_cast<UIKeyboardHIDUsage>(key_value);
  switch (key) {
    /* Alphabetic keys. */
    case UIKeyboardHIDUsageKeyboardA:
      return GHOST_kKeyA;
    case UIKeyboardHIDUsageKeyboardB:
      return GHOST_kKeyB;
    case UIKeyboardHIDUsageKeyboardC:
      return GHOST_kKeyC;
    case UIKeyboardHIDUsageKeyboardD:
      return GHOST_kKeyD;
    case UIKeyboardHIDUsageKeyboardE:
      return GHOST_kKeyE;
    case UIKeyboardHIDUsageKeyboardF:
      return GHOST_kKeyF;
    case UIKeyboardHIDUsageKeyboardG:
      return GHOST_kKeyG;
    case UIKeyboardHIDUsageKeyboardH:
      return GHOST_kKeyH;
    case UIKeyboardHIDUsageKeyboardI:
      return GHOST_kKeyI;
    case UIKeyboardHIDUsageKeyboardJ:
      return GHOST_kKeyJ;
    case UIKeyboardHIDUsageKeyboardK:
      return GHOST_kKeyK;
    case UIKeyboardHIDUsageKeyboardL:
      return GHOST_kKeyL;
    case UIKeyboardHIDUsageKeyboardM:
      return GHOST_kKeyM;
    case UIKeyboardHIDUsageKeyboardN:
      return GHOST_kKeyN;
    case UIKeyboardHIDUsageKeyboardO:
      return GHOST_kKeyO;
    case UIKeyboardHIDUsageKeyboardP:
      return GHOST_kKeyP;
    case UIKeyboardHIDUsageKeyboardQ:
      return GHOST_kKeyQ;
    case UIKeyboardHIDUsageKeyboardR:
      return GHOST_kKeyR;
    case UIKeyboardHIDUsageKeyboardS:
      return GHOST_kKeyS;
    case UIKeyboardHIDUsageKeyboardT:
      return GHOST_kKeyT;
    case UIKeyboardHIDUsageKeyboardU:
      return GHOST_kKeyU;
    case UIKeyboardHIDUsageKeyboardV:
      return GHOST_kKeyV;
    case UIKeyboardHIDUsageKeyboardW:
      return GHOST_kKeyW;
    case UIKeyboardHIDUsageKeyboardX:
      return GHOST_kKeyX;
    case UIKeyboardHIDUsageKeyboardY:
      return GHOST_kKeyY;
    case UIKeyboardHIDUsageKeyboardZ:
      return GHOST_kKeyZ;

    /* Numeric keys. */
    case UIKeyboardHIDUsageKeyboard0:
      return GHOST_kKey0;
    case UIKeyboardHIDUsageKeyboard1:
      return GHOST_kKey1;
    case UIKeyboardHIDUsageKeyboard2:
      return GHOST_kKey2;
    case UIKeyboardHIDUsageKeyboard3:
      return GHOST_kKey3;
    case UIKeyboardHIDUsageKeyboard4:
      return GHOST_kKey4;
    case UIKeyboardHIDUsageKeyboard5:
      return GHOST_kKey5;
    case UIKeyboardHIDUsageKeyboard6:
      return GHOST_kKey6;
    case UIKeyboardHIDUsageKeyboard7:
      return GHOST_kKey7;
    case UIKeyboardHIDUsageKeyboard8:
      return GHOST_kKey8;
    case UIKeyboardHIDUsageKeyboard9:
      return GHOST_kKey9;

    /* Punctuation / Symbols. */
    case UIKeyboardHIDUsageKeyboardHyphen:
      return GHOST_kKeyMinus;
    case UIKeyboardHIDUsageKeyboardEqualSign:
      return GHOST_kKeyEqual;
    case UIKeyboardHIDUsageKeyboardOpenBracket:
      return GHOST_kKeyLeftBracket;
    case UIKeyboardHIDUsageKeyboardCloseBracket:
      return GHOST_kKeyRightBracket;
    case UIKeyboardHIDUsageKeyboardBackslash:
      return GHOST_kKeyBackslash;
    case UIKeyboardHIDUsageKeyboardSemicolon:
      return GHOST_kKeySemicolon;
    case UIKeyboardHIDUsageKeyboardQuote:
      return GHOST_kKeyQuote;
    case UIKeyboardHIDUsageKeyboardGraveAccentAndTilde:
      return GHOST_kKeyAccentGrave;
    case UIKeyboardHIDUsageKeyboardComma:
      return GHOST_kKeyComma;
    case UIKeyboardHIDUsageKeyboardPeriod:
      return GHOST_kKeyPeriod;
    case UIKeyboardHIDUsageKeyboardSlash:
      return GHOST_kKeySlash;

    /* Function keys */
    case UIKeyboardHIDUsageKeyboardF1:
      return GHOST_kKeyF1;
    case UIKeyboardHIDUsageKeyboardF2:
      return GHOST_kKeyF2;
    case UIKeyboardHIDUsageKeyboardF3:
      return GHOST_kKeyF3;
    case UIKeyboardHIDUsageKeyboardF4:
      return GHOST_kKeyF4;
    case UIKeyboardHIDUsageKeyboardF5:
      return GHOST_kKeyF5;
    case UIKeyboardHIDUsageKeyboardF6:
      return GHOST_kKeyF6;
    case UIKeyboardHIDUsageKeyboardF7:
      return GHOST_kKeyF7;
    case UIKeyboardHIDUsageKeyboardF8:
      return GHOST_kKeyF8;
    case UIKeyboardHIDUsageKeyboardF9:
      return GHOST_kKeyF9;
    case UIKeyboardHIDUsageKeyboardF10:
      return GHOST_kKeyF10;
    case UIKeyboardHIDUsageKeyboardF11:
      return GHOST_kKeyF11;
    case UIKeyboardHIDUsageKeyboardF12:
      return GHOST_kKeyF12;
    case UIKeyboardHIDUsageKeyboardF13:
      return GHOST_kKeyF13;
    case UIKeyboardHIDUsageKeyboardF14:
      return GHOST_kKeyF14;
    case UIKeyboardHIDUsageKeyboardF15:
      return GHOST_kKeyF15;
    case UIKeyboardHIDUsageKeyboardF16:
      return GHOST_kKeyF16;
    case UIKeyboardHIDUsageKeyboardF17:
      return GHOST_kKeyF17;
    case UIKeyboardHIDUsageKeyboardF18:
      return GHOST_kKeyF18;
    case UIKeyboardHIDUsageKeyboardF19:
      return GHOST_kKeyF19;
    case UIKeyboardHIDUsageKeyboardF20:
      return GHOST_kKeyF20;
    case UIKeyboardHIDUsageKeyboardF21:
      return GHOST_kKeyF21;
    case UIKeyboardHIDUsageKeyboardF22:
      return GHOST_kKeyF22;
    case UIKeyboardHIDUsageKeyboardF23:
      return GHOST_kKeyF23;
    case UIKeyboardHIDUsageKeyboardF24:
      return GHOST_kKeyF24;

    /* Modifier keys. */
    case UIKeyboardHIDUsageKeyboardLeftControl:
      return GHOST_kKeyLeftControl;
    case UIKeyboardHIDUsageKeyboardLeftShift:
      return GHOST_kKeyLeftShift;
    case UIKeyboardHIDUsageKeyboardLeftAlt:
      return GHOST_kKeyLeftAlt;
    case UIKeyboardHIDUsageKeyboardLeftGUI:
      return GHOST_kKeyLeftOS;
    case UIKeyboardHIDUsageKeyboardRightControl:
      return GHOST_kKeyRightControl;
    case UIKeyboardHIDUsageKeyboardRightShift:
      return GHOST_kKeyRightShift;
    case UIKeyboardHIDUsageKeyboardRightAlt:
      return GHOST_kKeyRightAlt;
    case UIKeyboardHIDUsageKeyboardRightGUI:
      return GHOST_kKeyRightOS;

    /* Arrow keys */
    case UIKeyboardHIDUsageKeyboardLeftArrow:
      return GHOST_kKeyLeftArrow;
    case UIKeyboardHIDUsageKeyboardRightArrow:
      return GHOST_kKeyRightArrow;
    case UIKeyboardHIDUsageKeyboardUpArrow:
      return GHOST_kKeyUpArrow;
    case UIKeyboardHIDUsageKeyboardDownArrow:
      return GHOST_kKeyDownArrow;

    /* Control keys. */
    case UIKeyboardHIDUsageKeyboardEscape:
      return GHOST_kKeyEsc;
    case UIKeyboardHIDUsageKeyboardSpacebar:
      return GHOST_kKeySpace;
    case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
      return GHOST_kKeyBackSpace;
    case UIKeyboardHIDUsageKeyboardTab:
      return GHOST_kKeyTab;
    case UIKeyboardHIDUsageKeyboardCapsLock:
      return GHOST_kKeyCapsLock;

    case UIKeyboardHIDUsageKeyboardReturnOrEnter:
    case UIKeyboardHIDUsageKeyboardReturn:
      return GHOST_kKeyEnter;

    case UIKeyboardHIDUsageKeyboardInsert:
      return GHOST_kKeyInsert;
    case UIKeyboardHIDUsageKeyboardDeleteForward:
      return GHOST_kKeyDelete;
    case UIKeyboardHIDUsageKeyboardPrintScreen:
      return GHOST_kKeyPrintScreen;
    case UIKeyboardHIDUsageKeyboardScrollLock:
      return GHOST_kKeyScrollLock;
    case UIKeyboardHIDUsageKeyboardPause:
      return GHOST_kKeyPause;

    case UIKeyboardHIDUsageKeyboardHome:
      return GHOST_kKeyHome;
    case UIKeyboardHIDUsageKeyboardEnd:
      return GHOST_kKeyEnd;
    case UIKeyboardHIDUsageKeyboardPageUp:
      return GHOST_kKeyUpPage;
    case UIKeyboardHIDUsageKeyboardPageDown:
      return GHOST_kKeyDownPage;

    case UIKeyboardHIDUsageKeyboardApplication:
      return GHOST_kKeyApp;
    case UIKeyboardHIDUsageKeyboardClear:
      return GHOST_kKeyClear;

    /* Numpad. */
    case UIKeyboardHIDUsageKeypad0:
      return GHOST_kKeyNumpad0;
    case UIKeyboardHIDUsageKeypad1:
      return GHOST_kKeyNumpad1;
    case UIKeyboardHIDUsageKeypad2:
      return GHOST_kKeyNumpad2;
    case UIKeyboardHIDUsageKeypad3:
      return GHOST_kKeyNumpad3;
    case UIKeyboardHIDUsageKeypad4:
      return GHOST_kKeyNumpad4;
    case UIKeyboardHIDUsageKeypad5:
      return GHOST_kKeyNumpad5;
    case UIKeyboardHIDUsageKeypad6:
      return GHOST_kKeyNumpad6;
    case UIKeyboardHIDUsageKeypad7:
      return GHOST_kKeyNumpad7;
    case UIKeyboardHIDUsageKeypad8:
      return GHOST_kKeyNumpad8;
    case UIKeyboardHIDUsageKeypad9:
      return GHOST_kKeyNumpad9;

    case UIKeyboardHIDUsageKeypadSlash:
      return GHOST_kKeyNumpadSlash;
    case UIKeyboardHIDUsageKeypadAsterisk:
      return GHOST_kKeyNumpadAsterisk;
    case UIKeyboardHIDUsageKeypadHyphen:
      return GHOST_kKeyNumpadMinus;
    case UIKeyboardHIDUsageKeypadPlus:
      return GHOST_kKeyNumpadPlus;
    case UIKeyboardHIDUsageKeypadPeriod:
      return GHOST_kKeyNumpadPeriod;

    case UIKeyboardHIDUsageKeypadNumLock:
      return GHOST_kKeyNumLock;
    case UIKeyboardHIDUsageKeypadEnter:
      return GHOST_kKeyNumpadEnter;

    /* Additional international keys. */
    case UIKeyboardHIDUsageKeyboardNonUSPound:
      /* On Apple ISO keyboard, this is the section key (§/±), no equivalent in our case. */
      return GHOST_kKeyUnknown;
    case UIKeyboardHIDUsageKeyboardNonUSBackslash:
      /* ISO backslash, bottom left of the enter key. */
      return GHOST_kKeyBackslash;
    case UIKeyboardHIDUsageKeyboardInternational1:
      /* From USB specs: Corresponds to Keyboard Non-US (/) / (?) used on Brazilian keyboards with
       *                 an additional bottom row key, located left of the shorter right-shift key.
       */
      return GHOST_kKeySlash;

    /* Unmapped / no-equivalent. */
    case UIKeyboardHIDUsageKeypadEqualSign:
    case UIKeyboardHIDUsageKeypadEqualSignAS400:
    case UIKeyboardHIDUsageKeypadComma:
    case UIKeyboardHIDUsageKeyboardPower:
    case UIKeyboardHIDUsageKeyboardExecute:
    case UIKeyboardHIDUsageKeyboardHelp:
    case UIKeyboardHIDUsageKeyboardMenu:
    case UIKeyboardHIDUsageKeyboardSelect:
    case UIKeyboardHIDUsageKeyboardStop:
    case UIKeyboardHIDUsageKeyboardAgain:
    case UIKeyboardHIDUsageKeyboardUndo:
    case UIKeyboardHIDUsageKeyboardCut:
    case UIKeyboardHIDUsageKeyboardCopy:
    case UIKeyboardHIDUsageKeyboardPaste:
    case UIKeyboardHIDUsageKeyboardFind:
    case UIKeyboardHIDUsageKeyboardMute:
    case UIKeyboardHIDUsageKeyboardVolumeUp:
    case UIKeyboardHIDUsageKeyboardVolumeDown:
    case UIKeyboardHIDUsageKeyboardLockingCapsLock:
    case UIKeyboardHIDUsageKeyboardLockingNumLock:
    case UIKeyboardHIDUsageKeyboardLockingScrollLock:
    case UIKeyboardHIDUsageKeyboardInternational2:
    case UIKeyboardHIDUsageKeyboardInternational3:
    case UIKeyboardHIDUsageKeyboardInternational4:
    case UIKeyboardHIDUsageKeyboardInternational5:
    case UIKeyboardHIDUsageKeyboardInternational6:
    case UIKeyboardHIDUsageKeyboardInternational7:
    case UIKeyboardHIDUsageKeyboardInternational8:
    case UIKeyboardHIDUsageKeyboardInternational9:
    case UIKeyboardHIDUsageKeyboardLANG1:
    case UIKeyboardHIDUsageKeyboardLANG2:
    case UIKeyboardHIDUsageKeyboardLANG3:
    case UIKeyboardHIDUsageKeyboardLANG4:
    case UIKeyboardHIDUsageKeyboardLANG5:
    case UIKeyboardHIDUsageKeyboardLANG6:
    case UIKeyboardHIDUsageKeyboardLANG7:
    case UIKeyboardHIDUsageKeyboardLANG8:
    case UIKeyboardHIDUsageKeyboardLANG9:
    case UIKeyboardHIDUsageKeyboardAlternateErase:
    case UIKeyboardHIDUsageKeyboardSysReqOrAttention:
    case UIKeyboardHIDUsageKeyboardCancel:
    case UIKeyboardHIDUsageKeyboardPrior:
    case UIKeyboardHIDUsageKeyboardSeparator:
    case UIKeyboardHIDUsageKeyboardOut:
    case UIKeyboardHIDUsageKeyboardOper:
    case UIKeyboardHIDUsageKeyboardClearOrAgain:
    case UIKeyboardHIDUsageKeyboardCrSelOrProps:
    case UIKeyboardHIDUsageKeyboardExSel:
    case UIKeyboardHIDUsageKeyboardErrorRollOver:
    case UIKeyboardHIDUsageKeyboardPOSTFail:
    case UIKeyboardHIDUsageKeyboardErrorUndefined:
    case UIKeyboardHIDUsageKeyboard_Reserved:
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
