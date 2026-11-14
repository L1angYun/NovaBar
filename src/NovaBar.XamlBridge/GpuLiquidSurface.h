#pragma once
#include "GpuTexturePresenter.h"
#include "GpuInputClient.h"
#include "../NovaBar.Capture/GpuClientCanvasOptics.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <string>
#include <memory>
#include <vector>

namespace novabar {
namespace gpu_surface_detail {
using namespace capture;
using namespace capture::gpu;
inline bool SameRect(PixelRect a, PixelRect b) noexcept {
 return a.Left==b.Left&&a.Top==b.Top&&a.Right==b.Right&&a.Bottom==b.Bottom;
}
inline bool Admissible(Frame const& frame,uint64_t revision,uint32_t width,uint32_t height,uint64_t now) noexcept {
 return frame.Status==Status::Ready&&frame.Error==S_OK&&frame.PayloadFlags==(BaseCanvasPayload|GpuBasePayload)
  &&frame.Pixels.empty()&&!frame.Stride&&!frame.MotionSequence&&SameRect(frame.RenderedBounds,{})
  &&frame.Revision==revision&&frame.Width==width&&frame.Height==height&&ValidExpectedFrame(ExpectedFrame(frame),now);
}
inline bool PresentationFresh(bool connected,GpuPresentationTag const& shown,GpuExpectedFrame const& accepted,uint64_t now) noexcept {
 return connected&&ValidExpectedFrame(accepted,now)&&shown.Epoch==accepted.Revision
  &&shown.SourceGeneration==accepted.Generation&&shown.SourceSerial&&shown.SourceSerial<=accepted.SourceSerial
  &&capture::detail::Fresh(shown.PublishedTick,now);
}
// Rendering identity contains only fields actually consumed by the GPU optics
// or native mapping. Metadata heartbeat and request motion labels are not poses.
class PoseQueue {
 struct Pose {
  PixelRect Bounds{},Viewport{};double Radius=0,Lens=0,Dispersion=0,Specular=0,Width=0,Height=0;
  bool operator==(Pose const& b) const noexcept {
   return SameRect(Bounds,b.Bounds)&&SameRect(Viewport,b.Viewport)&&Radius==b.Radius&&Lens==b.Lens
    &&Dispersion==b.Dispersion&&Specular==b.Specular&&Width==b.Width&&Height==b.Height;
  }
 } m_pose;
 uint64_t m_serial=0;bool m_havePose=false,m_haveQueued=false;GpuPresentationTag m_queued{};
public:
 GpuPresentationTag Desired(Request const& request,double width,double height,PixelRect viewport,GpuExpectedFrame const& base) noexcept {
  Pose next{request.Bounds,viewport,request.Radius,request.Material.Lens,request.Material.Dispersion,request.Specular,width,height};
  if(!m_havePose||!(next==m_pose)){m_pose=next;m_havePose=true;++m_serial;}
  return {request.Revision,base.Generation,base.SourceSerial,m_serial,base.PublishedTick};
 }
 bool NeedsDraw(GpuPresentationTag const& tag) const noexcept {return !m_haveQueued||!(tag==m_queued);}
 void Queued(GpuPresentationTag const& tag) noexcept {m_queued=tag;m_haveQueued=true;}
 void Clear() noexcept {m_haveQueued=false;m_queued={};}
};
// This scheduler also runs against a fence/ring model in the headless tests.
// The draw callback queues commands on the same context as the private copy.
// Fresh tests the *displayed* output, not just the newest cached source.
template<class Presenter,class Draw,class Fresh>
HRESULT Schedule(Presenter& presenter,PoseQueue& poses,GpuPresentationTag const& desired,
 GpuPresentationMapping const& mapping,Draw&& draw,Fresh&& fresh) noexcept {
 auto clear=[&]{presenter.Clear();poses.Clear();};
 if(!presenter.UpdateMapping(mapping)){const auto error=presenter.LastError();clear();return error;}
 presenter.TryPresent(desired);
 if(FAILED(presenter.LastError())&&presenter.LastError()!=E_PENDING){const auto error=presenter.LastError();clear();return error;}
 if(presenter.Ready()&&!fresh())clear();
 if(poses.NeedsDraw(desired)) {
  GpuOutputLease lease;
  if(presenter.TryAcquireTarget(lease)) {
   const auto error=draw(lease);
   if(FAILED(error)) {
    if(!presenter.CancelDraw(lease)){const auto cancel=presenter.LastError();clear();return cancel;}
    clear();return error;
   }
   if(!presenter.QueueDraw(lease,desired)){const auto queueError=presenter.LastError();clear();return queueError;}
   poses.Queued(desired);
  }else if(presenter.LastError()!=E_PENDING){const auto error=presenter.LastError();clear();return error;}
 }
 // Re-poll even after Acquire returns pending: it cannot gate presentation.
 presenter.TryPresent(desired);const auto error=presenter.LastError();
 if(FAILED(error)&&error!=E_PENDING){clear();return error;}
 if(presenter.Ready()&&!fresh()){clear();return E_PENDING;}
 return error;
}
struct Pipeline {
 LUID Adapter{};winrt::com_ptr<ID3D11Device> Device;winrt::com_ptr<ID3D11DeviceContext> Context;
 std::unique_ptr<GpuClientCanvasOptics> Optics;
};
inline std::shared_ptr<Pipeline> AcquirePipeline(LUID luid) {
 struct Entry {LUID Adapter{};std::weak_ptr<Pipeline> Value;};
 // UI-thread local: sharing is safe only under the caller's serialized UI work.
 // Weak entries do not pin a device or a compiled shader after the final panel.
 static thread_local std::vector<Entry> pool;
 std::erase_if(pool,[](Entry const& item){return item.Value.expired();});
 for(auto const& entry:pool)if(entry.Adapter.LowPart==luid.LowPart&&entry.Adapter.HighPart==luid.HighPart)
  if(auto value=entry.Value.lock();value&&SUCCEEDED(value->Device->GetDeviceRemovedReason()))return value;
 winrt::com_ptr<IDXGIFactory1> factory;winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void()));
 winrt::com_ptr<IDXGIAdapter1> selected;
 for(UINT index=0;;++index){
  winrt::com_ptr<IDXGIAdapter1> adapter;const auto error=factory->EnumAdapters1(index,adapter.put());
  if(error==DXGI_ERROR_NOT_FOUND)break;winrt::check_hresult(error);
  DXGI_ADAPTER_DESC1 desc{};winrt::check_hresult(adapter->GetDesc1(&desc));
  if(desc.AdapterLuid.LowPart==luid.LowPart&&desc.AdapterLuid.HighPart==luid.HighPart){selected=std::move(adapter);break;}
 }
 if(!selected)winrt::throw_hresult(DXGI_ERROR_NOT_FOUND);
 auto value=std::make_shared<Pipeline>();value->Adapter=luid;
 winrt::check_hresult(D3D11CreateDevice(selected.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
  nullptr,0,D3D11_SDK_VERSION,value->Device.put(),nullptr,value->Context.put()));
 // Capability proofs precede creating shared inputs. There is exactly one
 // D3D device and its one immediate context; no hidden optics/presenter device.
 auto device5=value->Device.as<ID3D11Device5>();auto context4=value->Context.as<ID3D11DeviceContext4>();
 value->Optics=std::make_unique<GpuClientCanvasOptics>(value->Device.get(),value->Context.get());
 pool.push_back({luid,value});return value;
}
}

// Call all methods on the owning UI thread. Prepare belongs to the source poll
// and is the only method doing owner-file I/O. Observe attempts a private GPU
// copy and retains only explicit source metadata while its fence is pending;
// PollPending and Present can run on Rendering and never read transport files.
// BOOTSTRAP: attach CurrentBrush() to the native background immediately after
// Prepare, even while Ready()==false, so XamlCompositionBrushBase::OnConnected
// can run. Waiting for Ready before attachment would deadlock presentation.
class GpuLiquidSurface {
 using Compositor=winrt::Windows::UI::Composition::Compositor;
 using Input=capture::gpu::ClientGpuInputChannel;
 std::shared_ptr<gpu_surface_detail::Pipeline> m_pipeline;
 Input m_input;capture::gpu::GpuPrivateBaseCache m_cache;GpuTexturePresenter m_presenter;
 gpu_surface_detail::PoseQueue m_pose;
 capture::gpu::GpuExpectedFrame m_pending{};capture::gpu::GpuOwnerRecord m_owner{};
 Compositor m_compositor{nullptr};std::wstring m_directory;uint64_t m_target=0,m_epoch=0,m_revision=0;
 uint32_t m_width=0,m_height=0;DWORD m_thread=0;bool m_initialized=false;
 HRESULT m_error=E_PENDING;
 bool OwnerThread() noexcept {
  if(m_thread&&m_thread!=GetCurrentThreadId()){m_error=RPC_E_WRONG_THREAD;return false;}return true;
 }
 bool Fail(HRESULT error) noexcept {ClearSource();m_error=error;return false;}
 bool DisplayFresh(uint64_t now) const noexcept {
  return m_cache.Ready(now)&&capture::gpu::ValidOwner(m_owner,now)
   &&gpu_surface_detail::PresentationFresh(m_presenter.Ready(),m_presenter.CurrentTag(),m_cache.Metadata(),now);
 }
public:
 GpuLiquidSurface()=default;
 GpuLiquidSurface(GpuLiquidSurface const&)=delete;GpuLiquidSurface& operator=(GpuLiquidSurface const&)=delete;
 ~GpuLiquidSurface(){Reset();}
 bool Prepare(Compositor const& compositor,std::wstring const& directory,uint64_t targetId,uint64_t clientEpoch,
  uint64_t revision,uint32_t width,uint32_t height) noexcept {
  if(!OwnerThread())return false;
  try {
   if(!compositor||directory.empty()||!targetId||!clientEpoch||!revision||!width||!height||width>16384||height>16384
    ||uint64_t(width)*height>capture::MaxPixels)return Fail(E_INVALIDARG);
   capture::gpu::GpuOwnerRecord owner{};
   if(!capture::gpu::TryReadFreshGpuOwner(directory,GetTickCount64(),owner)) {
    ClearSource();m_owner={};m_error=E_PENDING;return false;
   }
   const bool different=m_pipeline&&(m_directory!=directory||m_target!=targetId||m_epoch!=clientEpoch
    ||m_pipeline->Adapter.LowPart!=owner.AdapterLow||m_pipeline->Adapter.HighPart!=owner.AdapterHigh
    ||FAILED(m_pipeline->Device->GetDeviceRemovedReason()));
   if(different)Reset();
   if(!m_pipeline){
    m_pipeline=gpu_surface_detail::AcquirePipeline({owner.AdapterLow,owner.AdapterHigh});m_thread=GetCurrentThreadId();
    m_directory=directory;m_target=targetId;m_epoch=clientEpoch;
   }
   if(m_compositor!=compositor){m_presenter.Reset();m_pose.Clear();m_compositor=compositor;}
   if(!m_presenter.Initialize(compositor,m_pipeline->Device.get(),m_pipeline->Context.get()))return Fail(m_presenter.LastError());
   if(!m_initialized){
    if(!m_input.Initialize(directory,targetId,clientEpoch,m_pipeline->Device.get(),m_pipeline->Context.get()))return Fail(m_input.LastError());
    m_initialized=true;
   }
   if(m_revision!=revision||m_width!=width||m_height!=height||!capture::gpu::SameOwner(m_owner,owner))ClearSource();
   m_revision=revision;m_width=width;m_height=height;m_owner=owner;
   if(!m_input.Offer(owner,revision,width,height))return Fail(m_input.LastError());
   if(!m_presenter.EnsureSize(width,height))return Fail(m_presenter.LastError());
   m_error=S_OK;return true;
  }catch(HRESULT error){return Fail(error);}catch(...){return Fail(winrt::to_hresult());}
 }
 bool Observe(capture::Frame const& frame,uint64_t now) noexcept {
  if(!OwnerThread())return false;
  if(!m_initialized||!gpu_surface_detail::Admissible(frame,m_revision,m_width,m_height,now))
   return Fail(frame.Error!=S_OK?frame.Error:HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
  const auto expected=capture::gpu::ExpectedFrame(frame);
  const auto admission=m_input.AdmitMetadata(expected,now);
  if(admission==capture::gpu::GpuMetadataAdmission::Obsolete){m_error=E_PENDING;return false;}
  if(admission!=capture::gpu::GpuMetadataAdmission::Current)return Fail(m_input.LastError());
  m_pending=expected;return PollPending(now);
 }
 bool PollPending(uint64_t now) noexcept {
  if(!OwnerThread())return false;
  m_input.PollRetirement();
  if(!m_initialized||!m_pending.SourceSerial){m_error=E_PENDING;return false;}
  if(!capture::gpu::ValidOwner(m_owner,now)||!capture::gpu::ValidExpectedFrame(m_pending,now))return Fail(E_PENDING);
  const auto admission=m_input.AdmitMetadata(m_pending,now);
  if(admission==capture::gpu::GpuMetadataAdmission::Obsolete){m_pending={};m_error=E_PENDING;return false;}
  if(admission!=capture::gpu::GpuMetadataAdmission::Current)return Fail(m_input.LastError());
  const auto result=m_input.TryCopyLatest(m_pending,m_cache,now);m_error=m_input.LastError();
  if(result==capture::gpu::GpuInputReadResult::Unavailable)return Fail(m_error);
  return result==capture::gpu::GpuInputReadResult::Copied||result==capture::gpu::GpuInputReadResult::Unchanged;
 }
 bool Present(capture::Request const& current,double widthDip,double heightDip,PixelRect viewport,uint64_t now) noexcept {
  if(!OwnerThread())return false;
  if(m_pipeline){const auto error=m_pipeline->Device->GetDeviceRemovedReason();if(FAILED(error))return Fail(error);}
  if(!m_pipeline||!m_initialized||!m_cache.Ready(now)||!capture::gpu::ValidOwner(m_owner,now)
   ||current.Kind!=capture::Kind::Panel||current.Revision!=m_revision||current.TargetId!=m_target
   ||(current.Flags&(capture::GpuBase|capture::WorldCanvas|capture::ClientOptics))!=(capture::GpuBase|capture::WorldCanvas|capture::ClientOptics))
   {ClearPresentation();return false;}
  auto const& base=m_cache.Metadata();auto desired=m_pose.Desired(current,widthDip,heightDip,viewport,base);
  auto draw=[&](GpuOutputLease const& lease) noexcept {
   PixelFrame geometry;geometry.Width=int(base.Width);geometry.Height=int(base.Height);geometry.Bounds=base.BitmapBounds;
   if(!m_pipeline->Optics->Render(m_cache.ShaderResource(),geometry,current.Bounds,current.Radius,current.Material.Lens,
     current.Material.Dispersion,current.Specular,viewport,lease.Target))return m_pipeline->Optics->LastError();
   return S_OK;
  };
  m_error=gpu_surface_detail::Schedule(m_presenter,m_pose,desired,{base.BitmapBounds,current.Bounds,widthDip,heightDip},
   draw,[&]{return DisplayFresh(now);});
  return DisplayFresh(now);
 }
 winrt::Windows::UI::Xaml::Media::Brush CurrentBrush() noexcept {
  auto brush=m_presenter.CurrentBrush();
  if(!brush&&FAILED(m_presenter.LastError())&&m_presenter.LastError()!=E_PENDING){
   const auto error=m_presenter.LastError();ClearPresentation();m_error=error;
  }
  return brush;
 }
 bool Ready(uint64_t now=GetTickCount64()) const noexcept {return DisplayFresh(now);}
 bool HasBase() const noexcept {return m_cache.Texture()&&m_cache.Metadata().SourceSerial;}
 uint64_t PublishedTick() const noexcept {return HasBase()?m_cache.Metadata().PublishedTick:0;}
 HRESULT LastError() const noexcept {return m_error;}
 void ClearPresentation() noexcept {m_presenter.Clear();m_pose.Clear();m_error=E_PENDING;}
 // Clearing the private base releases its old shared slot serial permanently.
 // Retire that generation so a static producer must publish into a new offer;
 // its same-serial heartbeat cannot rebuild a cache from the released slot.
 void ClearSource() noexcept {ClearPresentation();m_pending={};m_cache.Clear();m_input.RequestRetire();}
 void Reset() noexcept {
  ClearSource();m_input.Reset();m_presenter.Reset();m_pipeline.reset();m_compositor=nullptr;m_owner={};m_directory.clear();
  m_target=m_epoch=m_revision=0;m_width=m_height=0;m_thread=0;m_initialized=false;m_error=E_PENDING;
 }
};
}
