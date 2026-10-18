#include <rex/cvar.h>
#include <rex/ui/windowed_app.h>
#include "/work/PS5_Fable2/build/ps5/fable2-link-sweep/provider-batch18/ps5_ui.h"
#include <cstdio>
#include <thread>
#include <ps5platform/klog.h>
extern "C" int sceSystemServiceLoadExec(const char*,const char*const*);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceKernelUsleep(unsigned int);
static void cp(const char* s){std::fprintf(stderr,"[FABLE2-BOOT] %s\n",s);std::fflush(stderr);FILE*f=std::fopen("/app0/boot-results.txt","a");if(f){std::fprintf(f,"%s\n",s);std::fclose(f);}}
extern "C" void catchReturnFromMain(int status){char s[64];std::snprintf(s,sizeof(s),"PROCESS_END status=%d",status);cp(s);cp("SHELL_EXIT_REQUEST");std::fflush(nullptr);sceSystemServiceLoadExec("exit",nullptr);for(;;)sceKernelUsleep(100000);}
extern "C" void dump_guest_waits();
extern "C" void dump_command_processor();
extern "C" void sample_guest_main();
void poll_devloop_capture(rex::ui::WindowedApp*);
int capture_boot_image(rex::ui::WindowedApp*);
int main(){ps5_klog_capture_stderr("[FABLE2-BOOT] ");cp("BOUNDED_GAME_BOOT_BEGIN");
bool sync1=rex::cvar::SetFlagByName("async_shader_compilation","false");
bool sync2=rex::cvar::SetFlagByName("vulkan_pipeline_creation_threads","0");
bool page79=rex::cvar::SetFlagByName("clear_memory_page_state","false");
bool scalex157=rex::cvar::SetFlagByName("draw_resolution_scale_x","2");
bool scaley157=rex::cvar::SetFlagByName("draw_resolution_scale_y","2");
cp((scalex157&&scaley157)?"BUILD157 draw_resolution_scale=2":"BUILD157 draw_resolution_scale SET FAILED");
cp(rex::cvar::SetFlagByName("swap_post_effect","fxaa")?"BUILD157 swap_post_effect=fxaa":"BUILD157 swap_post_effect SET FAILED");
cp(page79?"BUILD102 clear_memory_page_state=false, no periodic snapshot":"BUILD102 clear_memory_page_state SET FAILED");
char shader_flags[100];std::snprintf(shader_flags,sizeof(shader_flags),"SYNCHRONOUS_SHADER_FLAGS=%d,%d",int(sync1),int(sync2));cp(shader_flags);
std::thread([](){for(int i=0;i<14400;++i){sceKernelUsleep(1000000);if(i%5==4)cp("WATCHDOG_HEARTBEAT");}cp("WATCHDOG_TIME_LIMIT_CONTROLLED_EXIT");catchReturnFromMain(90);}).detach();
auto*context=new rex::ui::Ps5AppContext();auto creator=rex::ui::GetWindowedAppCreator();if(!creator)return 10;
auto app=creator(*context);if(!app)return 11;cp("ACTUAL_FABLE2_APP_CREATED");
if(!app->OnInitialize()){cp("ERROR_APP_INITIALIZE");catchReturnFromMain(12);}cp("APP_INITIALIZE_COMPLETE");
sceSystemServiceHideSplashScreen();
for(unsigned i=0;i<864000;++i){context->Pump();if(i%4==0)poll_devloop_capture(app.get());sceKernelUsleep(16667);if(i%60==0)cp("UI_PUMP_HEARTBEAT");if(context->HasQuitFromUIThread()){cp("APP_REQUESTED_QUIT");catchReturnFromMain(0);}}
cp("BOUNDED_UI_PUMP_COMPLETE_CONTROLLED_EXIT");catchReturnFromMain(0);
}
