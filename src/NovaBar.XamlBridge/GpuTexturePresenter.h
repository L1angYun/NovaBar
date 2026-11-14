#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetCurrentTime
#include <d3d11_4.h>
#include <d3d12.h>
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000C
#include <windows.ui.composition.interop.h>
#pragma pop_macro("NTDDI_VERSION")
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <functional>
#include <memory>
#include "GlassPixels.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
namespace novabar {
struct GpuPresentationMapping { PixelRect Canvas{},Material{};double WidthDip=0,HeightDip=0; };
struct GpuPresentationTag {
 uint64_t Epoch=0, SourceGeneration=0, SourceSerial=0, PoseSerial=0, PublishedTick=0;
 bool operator==(GpuPresentationTag const& other) const noexcept {
  return Epoch==other.Epoch&&SourceGeneration==other.SourceGeneration&&SourceSerial==other.SourceSerial&&PoseSerial==other.PoseSerial;
 }
};
namespace gpu_presenter_detail {
struct FenceObservation { HRESULT Error=S_OK; bool HasFence=false; uint64_t Value=0,Completed=0; };
inline bool Available(FenceObservation const& fence) noexcept {
 return SUCCEEDED(fence.Error)&&fence.HasFence&&fence.Completed!=UINT64_MAX&&fence.Completed>=fence.Value;
}
struct Lease { uint32_t Slot=UINT32_MAX; uint64_t Generation=0,LeaseId=0; };
inline std::atomic<uint64_t> NextRingGeneration{0};
class OutputRing {
public:
 OutputRing() noexcept:m_generation(++NextRingGeneration){}
 struct Slot { bool Held=false,Valid=false;uint64_t LeaseId=0,Fence=0;GpuPresentationTag Tag{}; };
 bool Acquire(std::array<bool,3> const& released,uint64_t completed,Lease& result) noexcept {
  result={};if(completed==UINT64_MAX)return false;
  // A render-before-present callback must leave completed content to present.
  // Reserve the newest completed draw even when all three outputs are valid;
  // the caller can skip this draw and present it without blocking the GPU.
  int protectedSlot=-1;
  for(int i=0;i<3;++i)if(m_slots[i].Valid&&!m_slots[i].Held&&m_slots[i].Fence<=completed
    &&(protectedSlot<0||m_slots[i].Fence>m_slots[protectedSlot].Fence))protectedSlot=i;
  int selected=-1;
  for(int i=0;i<3;++i)if(i!=m_active&&i!=protectedSlot&&!m_slots[i].Held&&m_slots[i].Fence<=completed&&released[i]) {
   if(selected<0||(!m_slots[i].Valid&&m_slots[selected].Valid)
     ||(m_slots[i].Valid==m_slots[selected].Valid&&m_slots[i].Fence<m_slots[selected].Fence))selected=i;
  }
  if(selected<0)return false;
  auto& s=m_slots[selected];s.Held=true;s.Valid=false;s.LeaseId=++m_nextLease;result={uint32_t(selected),m_generation,s.LeaseId};return true;
 }
 bool Owns(Lease const& lease) const noexcept {
  return lease.Slot<3&&lease.Generation==m_generation&&m_slots[lease.Slot].Held&&m_slots[lease.Slot].LeaseId==lease.LeaseId;
 }
 bool Cancel(Lease const& lease,uint64_t fence) noexcept {
  if(!Owns(lease))return false;
  auto& s=m_slots[lease.Slot];s.Held=false;s.Valid=false;s.Fence=fence;return true;
 }
 bool Queue(Lease const& lease,GpuPresentationTag const& tag,uint64_t fence) noexcept {
  if(!Cancel(lease,fence))return false;
  if(!tag.Epoch||!tag.SourceGeneration||!tag.SourceSerial||!fence)return false;
  if(m_haveLast&&tag.Epoch==m_last.Epoch&&tag.SourceGeneration==m_last.SourceGeneration
    &&(tag.SourceSerial<m_last.SourceSerial||tag.PoseSerial<m_last.PoseSerial))return false;
  auto& s=m_slots[lease.Slot];s.Tag=tag;s.Valid=true;m_last=tag;m_haveLast=true;return true;
 }
 int LatestCompleted(GpuPresentationTag const& desired,uint64_t completed) const noexcept {
  if(!m_connected||completed==UINT64_MAX)return -1;
  int best=-1;
  for(int i=0;i<3;++i) {
   auto const& s=m_slots[i];auto const& t=s.Tag;
   if(!s.Valid||s.Held||s.Fence>completed||t.Epoch!=desired.Epoch||t.SourceGeneration!=desired.SourceGeneration
     ||t.SourceSerial>desired.SourceSerial||t.PoseSerial>desired.PoseSerial)continue;
   if(!AbovePresentationFloor(t))continue;
   // A producer may advance before its preceding client draw completes. Admit
   // the newest completed prior source, preserving its own PublishedTick and
   // the monotonic presentation floor instead of waiting for an exact serial.
   if(best<0||t.SourceSerial>m_slots[best].Tag.SourceSerial
     ||(t.SourceSerial==m_slots[best].Tag.SourceSerial&&(t.PoseSerial>m_slots[best].Tag.PoseSerial
       ||(t.PoseSerial==m_slots[best].Tag.PoseSerial&&s.Fence>m_slots[best].Fence))))best=i;
  }
  return best;
 }
 bool CanRetain(GpuPresentationTag const& desired) const noexcept {
  if(!Ready())return false;auto const& t=m_slots[m_active].Tag;
  return t.Epoch==desired.Epoch&&t.SourceGeneration==desired.SourceGeneration&&t.SourceSerial<=desired.SourceSerial&&t.PoseSerial<=desired.PoseSerial;
 }
 GpuPresentationTag CurrentTag() const noexcept {return Ready()?m_slots[m_active].Tag:GpuPresentationTag{};}
 void RenewPublishedTick(GpuPresentationTag const& accepted) noexcept {
  if(!Ready())return;auto& current=m_slots[m_active].Tag;
  if(current.Epoch==accepted.Epoch&&current.SourceGeneration==accepted.SourceGeneration&&current.SourceSerial==accepted.SourceSerial
    &&accepted.PublishedTick>current.PublishedTick)current.PublishedTick=accepted.PublishedTick;
 }
 void Present(int slot) noexcept {
  if(m_connected&&slot>=0&&slot<3&&m_slots[slot].Valid&&AbovePresentationFloor(m_slots[slot].Tag)) {
   m_active=slot;m_lastPresented=m_slots[slot].Tag;m_havePresented=true;
  }
 }
 void Connect() noexcept {m_connected=true;}
 void Disconnect() noexcept {m_connected=false;m_active=-1;}
 void Hide() noexcept {m_active=-1;}
 bool HasHeld() const noexcept {for(auto const& s:m_slots)if(s.Held)return true;return false;}
 void SealHeld(uint64_t fence) noexcept {for(auto& s:m_slots)if(s.Held){s.Held=false;s.Valid=false;s.Fence=fence;}}
 void Clear() noexcept {m_generation=++NextRingGeneration;m_active=-1;m_haveLast=false;m_havePresented=false;m_lastPresented={};for(auto& s:m_slots){s.Held=false;s.Valid=false;}}
 void Reset() noexcept {Clear();m_slots={};}
 bool Ready() const noexcept {return m_connected&&m_active>=0&&m_slots[m_active].Valid;}
 bool Idle(uint64_t completed) const noexcept {
  if(completed==UINT64_MAX)return false;
  for(auto const& s:m_slots)if(s.Held||s.Fence>completed)return false;return true;
 }
private:
 std::array<Slot,3> m_slots{};uint64_t m_generation,m_nextLease=0;
 GpuPresentationTag m_last{},m_lastPresented{};bool m_connected=false,m_haveLast=false,m_havePresented=false;int m_active=-1;
 bool AbovePresentationFloor(GpuPresentationTag const& tag) const noexcept {
  // Active binding is transient; presentation history survives disconnect/hide.
  return !m_havePresented||tag.Epoch!=m_lastPresented.Epoch||tag.SourceGeneration!=m_lastPresented.SourceGeneration
   ||(tag.SourceSerial>=m_lastPresented.SourceSerial&&tag.PoseSerial>=m_lastPresented.PoseSerial);
 }
};
namespace comp=winrt::Windows::UI::Composition;
namespace media=winrt::Windows::UI::Xaml::Media;
namespace num=winrt::Windows::Foundation::Numerics;
inline HRESULT ImageToLocal(GpuPresentationMapping const& mapping,uint32_t width,uint32_t height,num::float3x2& result) noexcept {
 auto const& c=mapping.Canvas;auto const& m=mapping.Material;
 const int64_t cw=int64_t(c.Right)-c.Left,ch=int64_t(c.Bottom)-c.Top,mw=int64_t(m.Right)-m.Left,mh=int64_t(m.Bottom)-m.Top;
 if(cw<=0||ch<=0||mw<=0||mh<=0||!width||!height||!std::isfinite(mapping.WidthDip)||!std::isfinite(mapping.HeightDip)
   ||mapping.WidthDip<=0||mapping.HeightDip<=0||mapping.WidthDip>16384||mapping.HeightDip>16384)return E_INVALIDARG;
 const double sx=double(cw)*mapping.WidthDip/(double(width)*mw),sy=double(ch)*mapping.HeightDip/(double(height)*mh);
 const double tx=double(int64_t(c.Left)-m.Left)*mapping.WidthDip/mw,ty=double(int64_t(c.Top)-m.Top)*mapping.HeightDip/mh;
 if(!std::isfinite(sx)||!std::isfinite(sy)||!std::isfinite(tx)||!std::isfinite(ty)
   ||sx<=0||sy<=0||sx>1e6||sy>1e6||std::abs(tx)>1e7||std::abs(ty)>1e7)return E_INVALIDARG;
 result={float(sx),0,0,float(sy),float(tx),float(ty)};return S_OK;
}
struct State {
 OutputRing Ring;comp::CompositionSurfaceBrush Surface{nullptr};HRESULT Error=E_PENDING;bool Mapping=false;
 std::function<void()> Changed;
 bool Ready() const noexcept{return Ring.Ready()&&Mapping&&SUCCEEDED(Error);}
 void Notify(){if(Changed)Changed();}
 void Withdraw() noexcept {
  try{if(Surface)Surface.Surface(nullptr);}catch(...){}
  try{Notify();}catch(...){}
 }
};
struct XamlBrush : media::XamlCompositionBrushBaseT<XamlBrush> {
 std::shared_ptr<State> Shared;comp::CompositionBrush Applied{nullptr};
 explicit XamlBrush(std::shared_ptr<State> state):Shared(std::move(state)){FallbackColor({0,0,0,0});}
 void Refresh() {
  comp::CompositionBrush next=nullptr;if(Shared->Ready())next=Shared->Surface;
  if(next!=Applied){CompositionBrush(next);Applied=next;}
 }
 void OnConnected() noexcept {
  try{Shared->Ring.Connect();Refresh();}
  catch(...){Shared->Error=winrt::to_hresult();Shared->Ring.Hide();Shared->Withdraw();}
 }
 void OnDisconnected() noexcept {
  Shared->Ring.Disconnect();Shared->Withdraw();
  try{CompositionBrush(nullptr);}catch(...){}Applied=nullptr;
 }
};
}

// UI-thread owner. The caller renders through the shared immediate context,
// queues that lease exactly once, and checks source freshness before TryPresent.
// PublishedTick comes only from accepted source metadata. CurrentTag lets the
// caller enforce the retained output's own TTL when a newer source is pending.
// Lease pointers are borrowed until Clear/EnsureSize/Reset. No waits or workers.
struct GpuOutputLease : gpu_presenter_detail::Lease {ID3D11RenderTargetView* Target=nullptr;};
class GpuTexturePresenter {
 using Compositor=winrt::Windows::UI::Composition::Compositor;
 using SurfaceBrushType=winrt::Windows::UI::Composition::CompositionSurfaceBrush;
 struct Output {
  winrt::com_ptr<ID3D11Texture2D> Texture;winrt::com_ptr<ID3D11RenderTargetView> Target;
  winrt::Windows::UI::Composition::CompositionTexture Surface{nullptr};
 };
public:
 GpuTexturePresenter()=default;
 GpuTexturePresenter(GpuTexturePresenter const&)=delete;
 GpuTexturePresenter& operator=(GpuTexturePresenter const&)=delete;
 ~GpuTexturePresenter(){Reset();}
 bool Initialize(Compositor const& compositor,ID3D11Device* device,ID3D11DeviceContext* context) noexcept {
  try {
   if(!compositor||!device||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return Fail(E_INVALIDARG);
   winrt::com_ptr<ID3D11Device> owner;context->GetDevice(owner.put());
   if(owner.get()!=device)return Fail(E_INVALIDARG);
   winrt::check_hresult(device->GetDeviceRemovedReason());
   if(!m_faulted&&m_compositor==compositor&&m_device.get()==device&&m_context.get()==context&&m_state&&m_readyFence){m_error=S_OK;m_state->Error=S_OK;return true;}
   Reset();m_state=std::make_shared<gpu_presenter_detail::State>();
   auto interop=compositor.as<ABI::Windows::UI::Composition::ICompositorInterop2>();BOOL supported=FALSE;
   winrt::check_hresult(interop->CheckCompositionTextureSupport(device,&supported));
   if(!supported)return Fail(DXGI_ERROR_UNSUPPORTED);
   winrt::com_ptr<ID3D11Device> retained;retained.copy_from(device);auto device5=retained.as<ID3D11Device5>();
   winrt::com_ptr<ID3D11DeviceContext> retainedContext;retainedContext.copy_from(context);auto context4=retainedContext.as<ID3D11DeviceContext4>();
   winrt::com_ptr<ID3D11Fence> fence;winrt::check_hresult(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,__uuidof(ID3D11Fence),fence.put_void()));
   auto brush=compositor.CreateSurfaceBrush();brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);
   brush.HorizontalAlignmentRatio(0);brush.VerticalAlignmentRatio(0);brush.Scale({1,1});brush.Offset({0,0});
   brush.BitmapInterpolationMode(winrt::Windows::UI::Composition::CompositionBitmapInterpolationMode::Linear);
   m_device=std::move(retained);m_context=std::move(retainedContext);m_context4=std::move(context4);
   m_readyFence=std::move(fence);m_interop=std::move(interop);m_compositor=compositor;m_state->Surface=brush;
   m_error=S_OK;m_state->Error=S_OK;return true;
  }catch(...){return Fail(winrt::to_hresult());}
 }
 bool EnsureSize(uint32_t width,uint32_t height) noexcept {
  try {
   if(m_faulted||!m_state||!m_device||!m_readyFence)return Fail(E_NOINTERFACE);
   if(!width||!height||width>16384||height>16384||uint64_t(width)*height>32ull*1024*1024)return Fail(E_INVALIDARG);
   winrt::check_hresult(m_device->GetDeviceRemovedReason());
   if(width==m_width&&height==m_height&&m_outputs[0].Texture){m_error=S_OK;m_state->Error=S_OK;return true;}
   // Detach first, then poll both ownership boundaries before replacing all
   // three outputs. Repeated resize cannot accumulate retiring output rings.
   Clear();const uint64_t completed=Completed();
   if(!m_state->Ring.Idle(completed)){m_error=E_PENDING;return false;}
   for(auto const& output:m_outputs)if(output.Surface&&!Released(output.Surface)){m_error=E_PENDING;return false;}
   m_outputs={};m_width=m_height=0;m_state->Ring.Reset();m_state->Mapping=false;
   D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
   desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
   desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
   for(auto& output:m_outputs) {
    winrt::check_hresult(m_device->CreateTexture2D(&desc,nullptr,output.Texture.put()));
    winrt::check_hresult(m_device->CreateRenderTargetView(output.Texture.get(),nullptr,output.Target.put()));
    winrt::check_hresult(m_interop->CreateCompositionTexture(output.Texture.get(),reinterpret_cast<ABI::Windows::UI::Composition::ICompositionTexture**>(winrt::put_abi(output.Surface))));
    output.Surface.AlphaMode(winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    output.Surface.SourceRect({0,0,int32_t(width),int32_t(height)});
    output.Surface.ColorSpace(winrt::Windows::Graphics::DirectX::DirectXColorSpace::RgbFullG22NoneP709);
   }
   m_width=width;m_height=height;m_error=S_OK;m_state->Error=S_OK;return true;
  }catch(...){return Fail(winrt::to_hresult());}
 }
 bool TryAcquireTarget(GpuOutputLease& lease) noexcept {
  lease={};
  try {
   if(m_faulted||!m_state||!m_width||!m_readyFence)return Fail(E_NOINTERFACE);
   const uint64_t completed=Completed();std::array<bool,3> released{};
   for(size_t i=0;i<3;++i)released[i]=Released(m_outputs[i].Surface);
   gpu_presenter_detail::Lease admitted;
   if(!m_state->Ring.Acquire(released,completed,admitted)){m_error=E_PENDING;return false;}
   static_cast<gpu_presenter_detail::Lease&>(lease)=admitted;lease.Target=m_outputs[admitted.Slot].Target.get();m_error=S_OK;return true;
  }catch(...){return Fail(winrt::to_hresult());}
 }
 bool QueueDraw(GpuOutputLease const& lease,GpuPresentationTag const& tag) noexcept {
  try {
   if(!Owns(lease))return Fail(E_INVALIDARG);
   const uint64_t value=Signal();
   if(!m_state->Ring.Queue(lease,tag,value))return Fail(E_INVALIDARG);
   m_error=S_OK;return true;
  }catch(...){return Fail(winrt::to_hresult());}
 }
 bool CancelDraw(GpuOutputLease const& lease) noexcept {
  try {if(!Owns(lease))return Fail(E_INVALIDARG);m_state->Ring.Cancel(lease,Signal());return true;}
  catch(...){return Fail(winrt::to_hresult());}
 }
 bool UpdateMapping(GpuPresentationMapping const& mapping) noexcept {
  try {
   if(!m_state||!m_state->Surface)return Fail(E_NOINTERFACE);
   gpu_presenter_detail::num::float3x2 matrix;
   winrt::check_hresult(gpu_presenter_detail::ImageToLocal(mapping,m_width,m_height,matrix));
   m_state->Surface.TransformMatrix(matrix);m_state->Mapping=true;m_state->Error=S_OK;m_error=S_OK;m_state->Notify();return true;
  }catch(...){if(m_state)m_state->Mapping=false;return Fail(winrt::to_hresult());}
 }
 bool TryPresent(GpuPresentationTag const& desired) noexcept {
  try {
   if(!m_state||!m_width||!m_readyFence)return Fail(E_NOINTERFACE);
   const int selected=m_state->Ring.LatestCompleted(desired,Completed());
   if(selected<0) {
    if(!m_state->Ring.CanRetain(desired)) {m_state->Ring.Hide();m_state->Withdraw();}
    m_error=E_PENDING;return false;
   }
   if(!m_state->Mapping){m_error=E_PENDING;return false;}
   if(m_state->Surface.Surface()!=m_outputs[selected].Surface)m_state->Surface.Surface(m_outputs[selected].Surface);
   m_state->Ring.Present(selected);m_state->Ring.RenewPublishedTick(desired);
   m_state->Error=S_OK;m_error=S_OK;m_state->Notify();return Ready();
  }catch(...){return Fail(winrt::to_hresult());}
 }
 winrt::Windows::UI::Xaml::Media::Brush CurrentBrush() noexcept {
  try {
   if(!m_state||!m_state->Surface)return nullptr;
   if(!m_xamlBrush) {
    auto brush=winrt::make_self<gpu_presenter_detail::XamlBrush>(m_state);auto weak=brush->get_weak();
    m_state->Changed=[weak]{if(auto alive=weak.get())alive->Refresh();};
    m_xamlBrush=brush.as<winrt::Windows::UI::Xaml::Media::Brush>();
   }
   return m_xamlBrush;
  }catch(...){Fail(winrt::to_hresult());return nullptr;}
 }
 SurfaceBrushType SurfaceBrush() const noexcept{return m_state?m_state->Surface:nullptr;}
 bool Ready() const noexcept{return m_state&&m_state->Ready();}
 GpuPresentationTag CurrentTag() const noexcept{return Ready()?m_state->Ring.CurrentTag():GpuPresentationTag{};}
 HRESULT LastError() const noexcept{return m_error;}
 void Clear() noexcept {
  // An acquired lease may already contain caller draw commands even when the
  // renderer subsequently rejects the frame. Seal those writes before reuse.
  if(m_state&&m_state->Ring.HasHeld()&&m_context4&&m_readyFence) {
   try{m_state->Ring.SealHeld(Signal());}catch(...){m_faulted=true;}
  }
  if(m_state){m_state->Ring.Clear();m_state->Error=E_PENDING;m_state->Withdraw();}m_error=E_PENDING;
 }
 void Reset() noexcept {
  Clear();if(m_state){m_state->Surface=nullptr;m_state->Mapping=false;try{m_state->Notify();}catch(...){}}
  m_xamlBrush=nullptr;m_state.reset();m_outputs={};m_readyFence=nullptr;m_context4=nullptr;m_context=nullptr;m_device=nullptr;
  m_interop=nullptr;m_compositor=nullptr;m_width=m_height=0;m_nextFence=0;m_error=E_PENDING;m_faulted=false;
 }
private:
 std::shared_ptr<gpu_presenter_detail::State> m_state;
 winrt::Windows::UI::Xaml::Media::Brush m_xamlBrush{nullptr};Compositor m_compositor{nullptr};
 winrt::com_ptr<ID3D11Device> m_device;winrt::com_ptr<ID3D11DeviceContext> m_context;winrt::com_ptr<ID3D11DeviceContext4> m_context4;
 winrt::com_ptr<ID3D11Fence> m_readyFence;winrt::com_ptr<ABI::Windows::UI::Composition::ICompositorInterop2> m_interop;
 std::array<Output,3> m_outputs{};uint32_t m_width=0,m_height=0;uint64_t m_nextFence=0;HRESULT m_error=E_PENDING;bool m_faulted=false;
 bool Fail(HRESULT error) noexcept {Clear();m_error=error;if(m_state)m_state->Error=error;return false;}
 uint64_t Completed() {
  winrt::check_hresult(m_device->GetDeviceRemovedReason());const auto value=m_readyFence->GetCompletedValue();
  if(value==UINT64_MAX)winrt::throw_hresult(DXGI_ERROR_DEVICE_REMOVED);return value;
 }
 bool Released(winrt::Windows::UI::Composition::CompositionTexture const& surface) {
  if(!surface)return false;auto interop=surface.as<ABI::Windows::UI::Composition::ICompositionTextureInterop>();
  gpu_presenter_detail::FenceObservation observed;winrt::com_ptr<ID3D11Fence> fence;
  observed.Error=interop->GetAvailableFence(&observed.Value,__uuidof(ID3D11Fence),fence.put_void());
  if(observed.Error==E_NOINTERFACE) {
   winrt::com_ptr<ID3D12Fence> other;observed.Error=interop->GetAvailableFence(&observed.Value,__uuidof(ID3D12Fence),other.put_void());
   winrt::check_hresult(observed.Error);observed.HasFence=bool(other);if(other)observed.Completed=other->GetCompletedValue();
  }else {winrt::check_hresult(observed.Error);observed.HasFence=bool(fence);if(fence)observed.Completed=fence->GetCompletedValue();}
  if(observed.HasFence&&observed.Completed==UINT64_MAX)winrt::throw_hresult(DXGI_ERROR_DEVICE_REMOVED);
  return gpu_presenter_detail::Available(observed);
 }
 bool Owns(GpuOutputLease const& lease) const noexcept {
  return m_state&&m_state->Ring.Owns(lease)&&lease.Target==m_outputs[lease.Slot].Target.get();
 }
 uint64_t Signal() {
  if(m_nextFence>=UINT64_MAX-1)winrt::throw_hresult(E_UNEXPECTED);
  const uint64_t value=++m_nextFence;winrt::check_hresult(m_context4->Signal(m_readyFence.get(),value));
  m_context->Flush();return value; // Submission only: Completed() is the readiness proof.
 }
};
}
