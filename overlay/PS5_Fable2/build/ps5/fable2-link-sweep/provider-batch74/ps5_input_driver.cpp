#include <rex/input/input_system.h>
#include <rex/input/device_assignment.h>
#include <rex/input/nop/nop_input_driver.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <algorithm>
extern "C" {
int scePadSetVibration(int,const void*);
int scePadSetVibrationMode(int,int);
int scePadInit();int scePadOpen(int,int,int,const void*);int scePadRead(int,void*,int);int scePadClose(int);
int sceUserServiceInitialize(const void*);int sceUserServiceGetInitialUser(int*);
}
namespace rex::input {
namespace {
struct NativeTouch {uint16_t x,y;uint8_t id,reserved[3];};
struct NativePad {
 uint32_t buttons;uint8_t lx,ly,rx,ry,l2,r2,reserved0[2];
 float orientation[4],acceleration[3],angular_velocity[3];
 uint8_t touch_count,reserved1[7];NativeTouch touch[2];
 uint8_t connected,reserved2[3];uint64_t timestamp;uint8_t extension[16],connected_count,reserved3[15];
};
static_assert(sizeof(NativePad)==120);static_assert(offsetof(NativePad,connected)==0x4c);
constexpr DeviceId kDevice=static_cast<DeviceId>(0x50533500);
int16_t axis(uint8_t value,bool inverse=false){int d=int(value)-128;int v=d>=0?d*32767/127:d*256;if(inverse)v=-v;return int16_t(std::max(-32768,std::min(32767,v)));}
class Ps5InputDriver final:public InputDriver {
 public:
 Ps5InputDriver():InputDriver(nullptr,0){}
 ~Ps5InputDriver()override{if(handle_>=0){const uint8_t zero[2]={0,0};scePadSetVibration(handle_,zero);scePadClose(handle_);}}
 X_STATUS Setup()override {
  int user_status=sceUserServiceInitialize(nullptr),pad_status=scePadInit();
  std::fprintf(stderr,"[FABLE2-INPUT] SERVICES user=0x%08x pad=0x%08x\n",unsigned(user_status),unsigned(pad_status));std::fflush(stderr);
  Poll();return X_STATUS_SUCCESS;
 }
 void EnumerateDevices(std::vector<DeviceInfo>& out)override {
  std::lock_guard<std::mutex> lock(mutex_);Poll();if(!connected_){vibration_valid_=false;return;}
  DeviceInfo info;info.id=kDevice;info.name="PS5 controller";info.guid="ps5-native-primary";out.push_back(info);
 }
 X_RESULT GetDeviceState(DeviceId id,X_INPUT_STATE* out)override {
  std::lock_guard<std::mutex> lock(mutex_);Poll();if(id!=kDevice||!connected_)return X_ERROR_DEVICE_NOT_CONNECTED;
  if(out)*out=state_;return X_ERROR_SUCCESS;
 }
 X_RESULT GetDeviceCapabilities(DeviceId id,uint32_t,X_INPUT_CAPABILITIES* caps)override {
  if(id!=kDevice)return X_ERROR_DEVICE_NOT_CONNECTED;
  if(caps){std::memset(caps,0,sizeof(*caps));caps->type=1;caps->sub_type=1;caps->gamepad.buttons=0xf3ff;caps->gamepad.left_trigger=255;caps->gamepad.right_trigger=255;caps->gamepad.thumb_lx=32767;caps->gamepad.thumb_ly=32767;caps->gamepad.thumb_rx=32767;caps->gamepad.thumb_ry=32767;caps->vibration.left_motor_speed=65535;caps->vibration.right_motor_speed=65535;}return X_ERROR_SUCCESS;
 }
 X_RESULT SetDeviceVibration(DeviceId id,X_INPUT_VIBRATION* vibration)override {
  std::lock_guard<std::mutex> lock(mutex_);
  if(id!=kDevice||handle_<0||!connected_)return X_ERROR_DEVICE_NOT_CONNECTED;
  if(!vibration)return X_ERROR_BAD_ARGUMENTS;
  const uint16_t left=vibration->left_motor_speed,right=vibration->right_motor_speed;
  const uint8_t motors[2]={uint8_t((uint32_t(left)*255+32767)/65535),uint8_t((uint32_t(right)*255+32767)/65535)};
  if(vibration_valid_ && motors[0]==last_motors_[0]&&motors[1]==last_motors_[1])return X_ERROR_SUCCESS;
  const int result=scePadSetVibration(handle_,motors);
  if(result<0){if(!vibration_error_){std::fprintf(stderr,"[FABLE2-RUMBLE73] failed=0x%08x\n",unsigned(result));std::fflush(stderr);vibration_error_=true;}return X_ERROR_DEVICE_NOT_CONNECTED;}
  last_motors_[0]=motors[0];last_motors_[1]=motors[1];vibration_valid_=true;vibration_error_=false;
  if(vibration_reports_++<24){std::fprintf(stderr,"[FABLE2-RUMBLE73] left=%u right=%u native=%u,%u\n",unsigned(left),unsigned(right),unsigned(motors[0]),unsigned(motors[1]));std::fflush(stderr);}
  return X_ERROR_SUCCESS;
 }

 X_RESULT GetDeviceKeystroke(DeviceId id,uint32_t,X_INPUT_KEYSTROKE*)override{return id==kDevice?X_ERROR_EMPTY:X_ERROR_DEVICE_NOT_CONNECTED;}
 private:
 void Poll(){
  if(handle_<0){auto now=std::chrono::steady_clock::now();if(now<retry_)return;retry_=now+std::chrono::seconds(1);
   int user=-1;if(sceUserServiceGetInitialUser(&user)<0)return;handle_=scePadOpen(user,0,0,nullptr);
   if(handle_<0)return;
   const int mode74=scePadSetVibrationMode(handle_,2);
   vibration_valid_=false;
   std::fprintf(stderr,"[FABLE2-RUMBLE74] compatible_mode result=0x%08x handle=%d\n",unsigned(mode74),handle_);std::fflush(stderr);
   std::fprintf(stderr,"[FABLE2-INPUT] OPEN handle=%d\n",handle_);std::fflush(stderr);
  }
  NativePad samples[64]{};int count=scePadRead(handle_,samples,64);
  if(count<0){vibration_valid_=false;connected_=false;scePadClose(handle_);handle_=-1;return;}if(count==0)return;
  if(count>64)count=64;const auto& sample=samples[count-1];bool was_connected=connected_;connected_=sample.connected!=0;
  if(!connected_)return;
  uint32_t buttons=sample.buttons&0x80000000u?0:sample.buttons;uint16_t mapped=0;
  const uint32_t source[]{0x10,0x40,0x80,0x20,0x8,0x100000,0x2,0x4,0x400,0x800,0x4000,0x2000,0x8000,0x1000};
  const uint16_t target[]{1,2,4,8,0x10,0x20,0x40,0x80,0x100,0x200,0x1000,0x2000,0x4000,0x8000};
  for(unsigned i=0;i<14;++i)if(buttons&source[i])mapped|=target[i];
  if(!was_connected||mapped!=uint16_t(state_.gamepad.buttons)){std::fprintf(stderr,"[FABLE2-INPUT] CONNECTED=%d XBOX_BUTTONS=0x%04x\n",int(connected_),mapped);std::fflush(stderr);}
  X_INPUT_GAMEPAD next{};next.buttons=mapped;
  if(!(sample.buttons&0x80000000u)){next.left_trigger=sample.l2;next.right_trigger=sample.r2;next.thumb_lx=axis(sample.lx);next.thumb_ly=axis(sample.ly,true);next.thumb_rx=axis(sample.rx);next.thumb_ry=axis(sample.ry,true);}
  if(std::memcmp(&next,&state_.gamepad,sizeof(next)))++packet_;state_.packet_number=packet_;state_.gamepad=next;
 }
 bool vibration_valid_=false,vibration_error_=false;uint8_t last_motors_[2]{};unsigned vibration_reports_=0;
 std::mutex mutex_;int handle_=-1;bool connected_=false;uint32_t packet_=0;X_INPUT_STATE state_{};std::chrono::steady_clock::time_point retry_{};
};
}
std::unique_ptr<system::IInputSystem> CreatePs5InputSystem(bool tool_mode){
 auto input=std::make_unique<InputSystem>(nullptr);
 if(!tool_mode){auto driver=std::make_unique<Ps5InputDriver>();if(driver->Setup()==X_STATUS_SUCCESS)input->AddDriver(std::move(driver));}
 input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr,1));input->SetDeviceAssignment(std::make_unique<SlotAssignment>());return input;
}
}

