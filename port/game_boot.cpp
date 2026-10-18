// Fable 2 (PS5): process entry. Sets the emulator options this port is tuned for, seeds the
// shader cache, creates the ReXGlue app and pumps its UI until the player quits.
//
// Differences from the development build (provider-batch157/game_boot.cpp):
//  - no 4-hour watchdog and no 4-hour cap on the UI loop (both were test-harness limits),
//  - no /app0/boot-results.txt log (klog only),
//  - shader cache seeding and recovery (cache_seed.cpp).
#include <rex/cvar.h>
#include <rex/ui/windowed_app.h>
#include "/work/PS5_Fable2/build/ps5/fable2-link-sweep/provider-batch18/ps5_ui.h"
#include <cstdio>
#include <ps5platform/klog.h>
extern "C" int sceSystemServiceLoadExec(const char*, const char* const*);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceKernelUsleep(unsigned int);
namespace fable2_cache {
void SeedOnBoot();
void StartShaderRecovery();
}
static void cp(const char* s) {
  std::fprintf(stderr, "[FABLE2-BOOT] %s\n", s);
  std::fflush(stderr);
}
extern "C" void catchReturnFromMain(int status) {
  char s[64];
  std::snprintf(s, sizeof(s), "PROCESS_END status=%d", status);
  cp(s);
  cp("SHELL_EXIT_REQUEST");
  std::fflush(nullptr);
  sceSystemServiceLoadExec("exit", nullptr);
  for (;;) sceKernelUsleep(100000);
}
void poll_devloop_capture(rex::ui::WindowedApp*);

int main() {
  ps5_klog_capture_stderr("[FABLE2-BOOT] ");
  cp("FABLE2_BOOT_BEGIN");
  // Pipelines are built synchronously: an asynchronous build skips the draw, which shows
  // as missing geometry for a frame.
  bool sync1 = rex::cvar::SetFlagByName("async_shader_compilation", "false");
  bool sync2 = rex::cvar::SetFlagByName("vulkan_pipeline_creation_threads", "0");
  // Page tracking survives a swap (true costs ~15x more uploads and stutters).
  bool pages = rex::cvar::SetFlagByName("clear_memory_page_state", "false");
  // 2x render scale: 2560x1440, free at the 30 FPS cap.
  bool scale_x = rex::cvar::SetFlagByName("draw_resolution_scale_x", "2");
  bool scale_y = rex::cvar::SetFlagByName("draw_resolution_scale_y", "2");
  bool fxaa = rex::cvar::SetFlagByName("swap_post_effect", "fxaa");
  char flags[160];
  std::snprintf(flags, sizeof(flags),
                "OPTIONS sync_shaders=%d,%d keep_page_state=%d scale2x=%d fxaa=%d", int(sync1),
                int(sync2), int(pages), int(scale_x && scale_y), int(fxaa));
  cp(flags);

  fable2_cache::SeedOnBoot();

  auto* context = new rex::ui::Ps5AppContext();
  auto creator = rex::ui::GetWindowedAppCreator();
  if (!creator) return 10;
  auto app = creator(*context);
  if (!app) return 11;
  cp("APP_CREATED");
  if (!app->OnInitialize()) {
    cp("ERROR_APP_INITIALIZE");
    catchReturnFromMain(12);
  }
  cp("APP_INITIALIZE_COMPLETE");
  fable2_cache::StartShaderRecovery();
  sceSystemServiceHideSplashScreen();
  for (unsigned i = 0;; ++i) {
    context->Pump();
    if (i % 4 == 0) poll_devloop_capture(app.get());
    sceKernelUsleep(16667);
    if (context->HasQuitFromUIThread()) {
      cp("APP_REQUESTED_QUIT");
      catchReturnFromMain(0);
    }
  }
}
