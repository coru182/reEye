// reEye: the PS5 app shell for ReXGlue: the entry point, the UI loop, and the one full-screen window, shown on
// VideoOut through the Vulkan presenter's VK_KHR_display surface (patches/rexglue-ps5.patch). Takes the place of
// ReXGlue's GTK entry point and window (windowed_app_main_posix.cpp, window_gtk.cpp).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#include <ps5platform/heap.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_ps5.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

namespace {
// /data/reeye: the game's files (game/), saves and settings (user/), and the log
constexpr const char* kDataDir = "/data/reeye";
constexpr const char* kGameDir = "/data/reeye/game";
constexpr const char* kUserDir = "/data/reeye/user";
constexpr const char* kLogFile = "/data/reeye/log.txt";
// the picture sent to VideoOut: the presenter takes the display mode closest to this
constexpr uint32_t kOutputWidth = 1920;
constexpr uint32_t kOutputHeight = 1080;
}  // namespace

namespace rex::ui {

class PS5Window;

class PS5WindowedAppContext final : public WindowedAppContext {
 public:
  void RunMainLoop();
  void SetWindow(PS5Window* window) { window_ = window; }
  PS5Window* window() const { return window_; }
  void Wake() { NotifyUILoopOfPendingFunctions(); }

 protected:
  void NotifyUILoopOfPendingFunctions() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake_ = true;
    }
    cv_.notify_one();
  }
  void PlatformQuitFromUIThread() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    cv_.notify_one();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool wake_ = false;
  bool quit_ = false;
  PS5Window* window_ = nullptr;
};

class PS5Window final : public Window {
 public:
  PS5Window(WindowedAppContext& app_context, const std::string_view title, uint32_t desired_logical_width,
            uint32_t desired_logical_height)
      : Window(app_context, title, desired_logical_width, desired_logical_height) {}

  ~PS5Window() override {
    EnterDestructor();
    auto& context = static_cast<PS5WindowedAppContext&>(app_context());
    if (context.window() == this) context.SetWindow(nullptr);
  }

  // From the UI loop: paint when the presenter has asked for it
  void PaintIfRequested() {
    if (paint_requested_.exchange(false)) {
      OnPaint(true);
      paints_++;
    }
  }
  uint32_t TakePaints() { return paints_.exchange(0); }

 protected:
  bool OpenImpl() override {
    auto& context = static_cast<PS5WindowedAppContext&>(app_context());
    context.SetWindow(this);
    // the PS5 has one screen: the window is always full screen, focused, and as large as the output
    OnDesiredFullscreenUpdate(true);
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(kOutputWidth, kOutputHeight, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) return true;
    OnFocusUpdate(true, destruction_receiver);
    return true;
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) return;
    OnAfterClose();
  }

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override {
    if (!(allowed_types & Surface::kTypeFlag_Ps5Display)) {
      REXLOG_ERROR("PS5Window: the graphics provider can't present to VideoOut");
      return nullptr;
    }
    return std::make_unique<Ps5DisplaySurface>(GetActualPhysicalWidth(), GetActualPhysicalHeight());
  }

  void RequestPaintImpl() override {
    paint_requested_ = true;
    static_cast<PS5WindowedAppContext&>(app_context()).Wake();
  }

 private:
  std::atomic<bool> paint_requested_{false};
  std::atomic<uint32_t> paints_{0};
};

void PS5WindowedAppContext::RunMainLoop() {
  // every 10 s the log says how many pictures went to the screen, for telling a stall from a black picture
  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!HasQuitFromUIThread()) {
    if (std::chrono::steady_clock::now() >= next_report) {
      next_report += std::chrono::seconds(10);
      REXLOG_INFO("reEye: {} paints in the last 10 s", window_ ? window_->TakePaints() : 0u);
    }
    {
      // wake for queued functions and paint requests; at the latest every frame, so a missed wake costs at most one
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(16), [this] { return wake_ || quit_; });
      wake_ = false;
    }
    ExecutePendingFunctionsFromUIThread();
    if (HasQuitFromUIThread()) break;
    if (window_) window_->PaintIfRequested();
  }
  // in case the loop ended some other way, so pending functions are not added pointlessly
  QuitFromUIThread();
}

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context, const std::string_view title,
                                       uint32_t desired_logical_width, uint32_t desired_logical_height) {
  return std::make_unique<PS5Window>(app_context, title, desired_logical_width ? desired_logical_width : kOutputWidth,
                                     desired_logical_height ? desired_logical_height : kOutputHeight);
}

}  // namespace rex::ui

// reEye: abort() from anywhere in the program (the heap finding a damaged block, std::terminate, an assert) writes
// /data/reeye/crash.txt first: the call stack as offsets in the linked program (look them up with llvm-symbolizer
// against build-ps5/link/llvm-pie.elf), the thread, and the heap's state. The heap may be damaged here, so nothing
// below allocates. The link wraps abort (build-ps5.sh).
extern "C" [[noreturn]] void __real_abort(void);
extern "C" int main(int argc, char** argv);

namespace {
struct Frames {
  uintptr_t pc[48];
  int count;
};
_Unwind_Reason_Code CollectFrame(struct _Unwind_Context* context, void* arg) {
  auto* f = static_cast<Frames*>(arg);
  if (f->count >= 48) return _URC_END_OF_STACK;
  f->pc[f->count++] = uintptr_t(_Unwind_GetIP(context));
  return _URC_NO_REASON;
}
}  // namespace

extern "C" [[noreturn]] void __wrap_abort(void) {
  static std::atomic<bool> once{false};
  if (!once.exchange(true)) {
    int fd = open("/data/reeye/crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
      char line[160];
      // the program's load address, from where main is now and where the link put it (the symbol list's address)
      int n = snprintf(line, sizeof line, "abort() called\nmain at %p\nthread %p\n", reinterpret_cast<void*>(&main),
                       reinterpret_cast<void*>(pthread_self()));
      write(fd, line, size_t(n));
      Frames frames{};
      _Unwind_Backtrace(CollectFrame, &frames);
      for (int i = 0; i < frames.count; ++i) {
        n = snprintf(line, sizeof line, "  #%d 0x%llx\n", i, static_cast<unsigned long long>(frames.pc[i]));
        write(fd, line, size_t(n));
      }
      struct ps5_heap_stats heap{};
      ps5_heap_stats(&heap);
      n = snprintf(line, sizeof line, "heap: %zu MB mapped, peak %zu MB, %u segments, %u arenas, %llu libc fallbacks\n",
                   heap.mapped_bytes >> 20, heap.peak_bytes >> 20, heap.segments, heap.arenas,
                   static_cast<unsigned long long>(heap.libc_fallbacks));
      write(fd, line, size_t(n));
      close(fd);
    }
  }
  __real_abort();
}

extern "C" int main(int argc, char** argv) {
  mkdir(kDataDir, 0777);
  mkdir(kUserDir, 0777);
  // the app's own folders, before any setting the player put in ge.toml (it may still change them)
  setenv("HOME", kDataDir, 1);
  std::vector<std::string> args_storage = {argc > 0 && argv[0] ? argv[0] : "reeye",
                                           std::string("--game_data_root=") + kGameDir,
                                           std::string("--user_data_root=") + kUserDir,
                                           std::string("--log_file=") + kLogFile,
                                           // written out every second, so a stall or crash leaves the last lines
                                           std::string("--log_flush_interval=1")};
  for (int i = 1; i < argc; ++i) args_storage.emplace_back(argv[i]);
  std::vector<char*> args;
  for (auto& a : args_storage) args.push_back(a.data());
  args.push_back(nullptr);

  auto remaining = rex::cvar::Init(int(args.size() - 1), args.data());
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  REXLOG_INFO("reEye: starting (game files in {}, saves and settings in {})", kGameDir, kUserDir);

  int result;
  {
    rex::ui::PS5WindowedAppContext app_context;
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) parsed[option_names[i]] = remaining[i];
    app->SetParsedArguments(std::move(parsed));

    if (app->OnInitialize()) {
      app_context.RunMainLoop();
      result = EXIT_SUCCESS;
    } else {
      REXLOG_ERROR("reEye: the app failed to start (see the lines above)");
      result = EXIT_FAILURE;
    }
    app->InvokeOnDestroy();
  }
  rex::ShutdownLogging();
  return result;
}
