#include <rex/audio/audio_driver.h>
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/audio/downmix.h>
#include <array>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "pcm72.h"
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t);
int sceAudioOutOutput(int32_t,const void*);
int sceAudioOutClose(int32_t);
int sceKernelUsleep(unsigned int);
}
namespace rex::audio::nop {
namespace {
std::once_flag init_once72;
int init_result72=-1;
class PS5AudioDriver72 final : public AudioDriver {
  static constexpr size_t kSamples=256,kChannels=2,kCapacity=64;
  using Frame=std::array<int16_t,kSamples*kChannels>;
 public:
  PS5AudioDriver72(memory::Memory* memory,rex::thread::Semaphore* semaphore,size_t index)
    :AudioDriver(memory),semaphore_(semaphore),index_(index){}
  bool Initialize() {
    std::call_once(init_once72,[]{init_result72=sceAudioOutInit();});
    if(init_result72<0 && init_result72!=static_cast<int>(0x8026000e)) {
      std::fprintf(stderr,"[FABLE2-AUDIO72] init failed=0x%08x\n",unsigned(init_result72));std::fflush(stderr);return false;
    }
    port_=sceAudioOutOpen(0xff,0,0,kSamples,48000,1);
    if(port_<0) {
      std::fprintf(stderr,"[FABLE2-AUDIO72] open failed=0x%08x client=%zu\n",unsigned(port_),index_);std::fflush(stderr);return false;
    }
    running_.store(true);
    const int error=pthread_create(&thread_,nullptr,ThreadEntry,this);
    if(error) {running_.store(false);sceAudioOutClose(port_);port_=-1;
      std::fprintf(stderr,"[FABLE2-AUDIO72] thread failed=%d\n",error);std::fflush(stderr);return false;}
    started_=true;
    std::fprintf(stderr,"[FABLE2-AUDIO72] open client=%zu port=%d rate=48000 channels=2 grain=256\n",index_,port_);std::fflush(stderr);return true;
  }
  ~PS5AudioDriver72() override {
    running_.store(false);
    if(started_)pthread_join(thread_,nullptr);
    if(port_>=0){sceAudioOutOutput(port_,nullptr);sceAudioOutClose(port_);}
  }
  void SubmitFrame(uint32_t frame_ptr) override {
    // Per-channel energy distinguishes missing guest music from an output mix issue.
    const float* raw73=memory_->TranslateVirtual<float*>(frame_ptr);
    double energy73[6]{};
    for(unsigned ch73=0;ch73<6;++ch73)for(unsigned i73=0;i73<kSamples;++i73){
      const float v73=fable2_ps5_audio::load_be_float(raw73+ch73*kSamples+i73);
      if(std::isfinite(v73))energy73[ch73]+=double(v73)*v73;
    }
    Frame frame;
    const auto fold=GetStereoFold();
    fable2_ps5_audio::stereo(memory_->TranslateVirtual<float*>(frame_ptr),frame.data(),kSamples,
      fold.center,fold.surround,fold.lfe,fold.scale,GetOutputGain());
    bool dropped=false;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      ++submitted_;
      for(unsigned c73=0;c73<6;++c73)channel_energy73_[c73]+=energy73[c73];
      if(submitted_%1875==0){
        std::fprintf(stderr,"[FABLE2-MUSIC73] client=%zu rms FL=%.5f FR=%.5f C=%.5f LFE=%.5f SL=%.5f SR=%.5f gain=%.3f\n",index_,
          std::sqrt(channel_energy73_[0]/480000),std::sqrt(channel_energy73_[1]/480000),std::sqrt(channel_energy73_[2]/480000),std::sqrt(channel_energy73_[3]/480000),std::sqrt(channel_energy73_[4]/480000),std::sqrt(channel_energy73_[5]/480000),GetOutputGain());std::fflush(stderr);
        for(auto& e73:channel_energy73_)e73=0;
      }
      if(count_==kCapacity){++dropped_;dropped=true;}
      else {queue_[(read_+count_)%kCapacity]=frame;++count_;}
    }
    // Never wait on hardware while AudioSystem::SubmitFrame owns the global lock.
    // The callback credit for an accepted frame is returned by the output thread.
    if(dropped)semaphore_->Release(1,nullptr);
  }
 private:
  static void* ThreadEntry(void* self){static_cast<PS5AudioDriver72*>(self)->Output();return nullptr;}
  void Output() {
    alignas(64) Frame buffers[2]{};
    unsigned slot=0;
    unsigned long long outputs=0,underruns=0,errors=0,nonzero=0;
    int peak=0;
    while(running_.load()) {
      auto& frame=buffers[slot];frame.fill(0);
      bool available=false;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if(count_){frame=queue_[read_];read_=(read_+1)%kCapacity;--count_;available=true;}
      }
      if(!available)++underruns;
      for(const auto sample:frame){const int magnitude=sample<0?-int(sample):int(sample);peak=std::max(peak,magnitude);if(sample)++nonzero;}
      const int result=sceAudioOutOutput(port_,frame.data());
      if(result<0){++errors;if(errors==1){std::fprintf(stderr,"[FABLE2-AUDIO72] output failed=0x%08x client=%zu\n",unsigned(result),index_);std::fflush(stderr);}sceKernelUsleep(5333);}
      if(available)semaphore_->Release(1,nullptr);
      slot^=1;
      if(++outputs%1875==0) {
        size_t queued;unsigned long long submitted,dropped;
        {std::lock_guard<std::mutex> lock(queue_mutex_);queued=count_;submitted=submitted_;dropped=dropped_;}
        std::fprintf(stderr,"[FABLE2-AUDIO72] client=%zu outputs=%llu submitted=%llu queued=%zu underruns=%llu dropped=%llu errors=%llu nonzero=%llu peak=%d\n",index_,outputs,submitted,queued,underruns,dropped,errors,nonzero,peak);std::fflush(stderr);
      }
    }
  }
  rex::thread::Semaphore* semaphore_;
  size_t index_;
  int port_=-1;
  pthread_t thread_{};
  bool started_=false;
  std::atomic<bool> running_{false};
  std::mutex queue_mutex_;
  std::array<Frame,kCapacity> queue_{};
  size_t read_=0,count_=0;
  double channel_energy73_[6]{};
  unsigned long long submitted_=0,dropped_=0;
};
}
std::unique_ptr<AudioSystem> NopAudioSystem::Create(runtime::FunctionDispatcher* dispatcher){return std::make_unique<NopAudioSystem>(dispatcher);}
NopAudioSystem::NopAudioSystem(runtime::FunctionDispatcher* dispatcher):AudioSystem(dispatcher){}
NopAudioSystem::~NopAudioSystem()=default;
X_STATUS NopAudioSystem::CreateDriver(size_t index,rex::thread::Semaphore* semaphore,AudioDriver** out_driver) {
  *out_driver=nullptr;
  auto driver=std::make_unique<PS5AudioDriver72>(memory(),semaphore,index);
  if(!driver->Initialize())return X_STATUS_UNSUCCESSFUL;
  *out_driver=driver.release();return X_STATUS_SUCCESS;
}
void NopAudioSystem::DestroyDriver(AudioDriver* driver){delete driver;}
}
