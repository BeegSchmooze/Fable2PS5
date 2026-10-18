#pragma once
#include <rex/ui/windowed_app_context.h>
#include <atomic>
namespace rex::ui {
class Ps5AppContext final : public WindowedAppContext {
 public:
  ~Ps5AppContext() override { if (!HasQuitFromUIThread()) QuitFromUIThread(); }
  void Pump() { wake_.store(false); ExecutePendingFunctionsFromUIThread(); }
 protected:
  void NotifyUILoopOfPendingFunctions() override { wake_.store(true); }
  void PlatformQuitFromUIThread() override { wake_.store(true); }
 private:
  std::atomic<bool> wake_{false};
};
}
