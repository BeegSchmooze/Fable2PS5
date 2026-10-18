#include <rex/ui/window.h>
#include <memory>
namespace rex::ui {
namespace {
class Ps5Surface final : public Surface {
 public:
  Ps5Surface(uint32_t width,uint32_t height):width_(width),height_(height){}
  TypeIndex GetType() const override { return kTypeIndex_PS5; }
 protected:
  bool GetSizeImpl(uint32_t& width,uint32_t& height) const override {width=width_;height=height_;return width&&height;}
 private:
  uint32_t width_,height_;
};
class Ps5Window final : public Window {
 public:
  Ps5Window(WindowedAppContext& ctx,std::string_view title,uint32_t w,uint32_t h):Window(ctx,title,w,h){}
  ~Ps5Window() override {alive_.reset();EnterDestructor();}
 protected:
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnDesiredFullscreenUpdate(true);
    OnActualSizeUpdate(GetDesiredLogicalWidth(),GetDesiredLogicalHeight(),receiver);
    OnFocusUpdate(true,receiver);
    return true;
  }
  void ApplyNewFullscreen() override {OnDesiredFullscreenUpdate(true);}
  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);OnBeforeClose(receiver);
    if(receiver.IsWindowDestroyed())return;
    OnAfterClose();
  }
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed) override {
    if(!(allowed&Surface::kTypeFlag_PS5))return nullptr;
    return std::make_unique<Ps5Surface>(GetActualPhysicalWidth(),GetActualPhysicalHeight());
  }
  void RequestPaintImpl() override {
    std::weak_ptr<int> alive=alive_;
    app_context().CallInUIThreadDeferred([alive,this]{if(alive.lock())OnPaint();});
  }
 private:
  std::shared_ptr<int> alive_=std::make_shared<int>(0);
};
}
std::unique_ptr<Window> Window::Create(WindowedAppContext& ctx,std::string_view title,uint32_t w,uint32_t h){
  if(!w||!h)return nullptr;
  return std::make_unique<Ps5Window>(ctx,title,w,h);
}
}
