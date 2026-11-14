#include "../GpuLiquidSurface.h"
#include <cstdio>
#include <limits>
#include <DispatcherQueue.h>
#include <winrt/Windows.System.h>
#include <string_view>
using namespace novabar;
using namespace novabar::gpu_surface_detail;
using namespace novabar::capture;
using namespace novabar::capture::gpu;
struct FakePresenter {
 std::string Calls;HRESULT Error=E_PENDING;bool Acquire=true,Mapping=true,Shown=false,Queue=true;
 int Draws=0;
 bool UpdateMapping(GpuPresentationMapping const&){Calls+='M';if(!Mapping)Error=E_INVALIDARG;return Mapping;}
 bool TryPresent(GpuPresentationTag const&){Calls+='P';Error=E_PENDING;return Shown;}
 bool Ready() const{return Shown;}
 bool TryAcquireTarget(GpuOutputLease&){Calls+='A';Error=E_PENDING;return Acquire;}
 bool QueueDraw(GpuOutputLease const&,GpuPresentationTag const&){Calls+='Q';Error=Queue?S_OK:E_FAIL;return Queue;}
 bool CancelDraw(GpuOutputLease const&){Calls+='X';return true;}
 void Clear(){Calls+='C';Shown=false;}
 HRESULT LastError()const{return Error;}
};
struct RingPresenter {
 gpu_presenter_detail::OutputRing Ring;
 uint64_t Completed=0, NextFence=0;
 HRESULT Error=S_OK;
 RingPresenter(){Ring.Connect();}
 bool UpdateMapping(GpuPresentationMapping const&){Error=S_OK;return true;}
 bool TryPresent(GpuPresentationTag const& desired){
  int selected=Ring.LatestCompleted(desired,Completed);
  if(selected<0){if(!Ring.CanRetain(desired))Ring.Hide();Error=E_PENDING;return false;}
  Ring.Present(selected);Ring.RenewPublishedTick(desired);Error=S_OK;return Ring.Ready();
 }
 bool Ready()const{return Ring.Ready();}
 bool TryAcquireTarget(GpuOutputLease& lease){gpu_presenter_detail::Lease l;if(!Ring.Acquire({true,true,true},Completed,l)){Error=E_PENDING;return false;}static_cast<gpu_presenter_detail::Lease&>(lease)=l;Error=S_OK;return true;}
 bool QueueDraw(GpuOutputLease const& lease,GpuPresentationTag const& tag){bool ok=Ring.Queue(lease,tag,++NextFence);Error=ok?S_OK:E_FAIL;return ok;}
 bool CancelDraw(GpuOutputLease const& lease){return Ring.Cancel(lease,++NextFence);}
 void Clear(){Ring.Clear();Error=E_PENDING;}
 HRESULT LastError()const{return Error;}
};

template<class Check> void StaticRecovery(Check&& check) {
 winrt::init_apartment(winrt::apartment_type::single_threaded);
 const auto directory=(std::filesystem::temp_directory_path()/(L"novabar-surface-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()))).wstring();
 try {
  std::filesystem::create_directories(directory);
  DispatcherQueueOptions options{sizeof(options),DQTYPE_THREAD_CURRENT,DQTAT_COM_STA};winrt::Windows::System::DispatcherQueueController queue{nullptr};
  winrt::check_hresult(CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
  winrt::Windows::UI::Composition::Compositor compositor;
  winrt::com_ptr<ID3D11Device> producer;winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,producer.put(),nullptr,context.put()));
  auto device5=producer.as<ID3D11Device5>();auto context4=context.as<ID3D11DeviceContext4>();
  const auto self=CurrentIdentity();const auto adapter=capture::gpu::input_detail::DeviceAdapter(producer.get());
  GpuOwnerWriter writer;winrt::check_bool(writer.Publish(directory,self,RandomNonce(),adapter));
  GpuLiquidSurface surface;winrt::check_bool(surface.Prepare(compositor,directory,72,4,7,64,32));
  GpuControlMapping control;winrt::check_bool(control.Open(capture::detail::Path(directory,self.Pid,72,L".gpu")));
  auto offered=[&]() -> GpuGenerationRecord* {for(auto& record:control.Get()->Records)if(Phase(record)==GpuPhase::Offered)return &record;return nullptr;};
  uint64_t oldGeneration=0;Frame predecessor;
  for(int cycle=0;cycle<2;++cycle){
   auto* record=offered();winrt::check_bool(record!=nullptr);const auto offer=record->Offer;
   check(cycle==0||offer.Generation!=oldGeneration,"source withdrawal creates new input generation for static serial recovery");
   oldGeneration=offer.Generation;
   if(cycle){
    check(!surface.Observe(predecessor,GetTickCount64())&&surface.LastError()==E_PENDING&&!Cancelled(*record),
     "old source before replacement import is ignored without cancelling offer");
    check(!surface.PollPending(GetTickCount64())&&!Cancelled(*record),"old source is not retained as pending replacement work");
   }
   winrt::check_bool(TryClaim(*record));
   if(cycle)check(!surface.Observe(predecessor,GetTickCount64())&&surface.LastError()==E_PENDING&&!Cancelled(*record),
    "old source during replacement claim is ignored without cancelling claim");
   winrt::com_ptr<ID3D11Texture2D> texture;winrt::com_ptr<ID3D11RenderTargetView> target;winrt::com_ptr<ID3D11Fence> ready,consumed;
   winrt::check_hresult(device5->OpenSharedResource1(reinterpret_cast<HANDLE>(uintptr_t(offer.TextureHandle[0])),__uuidof(ID3D11Texture2D),texture.put_void()));
   winrt::check_hresult(producer->CreateRenderTargetView(texture.get(),nullptr,target.put()));
   winrt::check_hresult(device5->OpenSharedFence(reinterpret_cast<HANDLE>(uintptr_t(offer.ReadyFenceHandle)),__uuidof(ID3D11Fence),ready.put_void()));
   winrt::check_hresult(device5->OpenSharedFence(reinterpret_cast<HANDLE>(uintptr_t(offer.ConsumedFenceHandle)),__uuidof(ID3D11Fence),consumed.put_void()));
   winrt::check_bool(CompleteImport(*record,S_OK));
   const float color[]{.15f,.3f,.45f,1};context->ClearRenderTargetView(target.get(),color);winrt::check_hresult(context4->Signal(ready.get(),1));context->Flush();
   Frame publication;publication.Status=Status::Ready;publication.PayloadFlags=BaseCanvasPayload|GpuBasePayload;
   publication.Revision=7;publication.Generation=offer.Generation;publication.Serial=77;publication.ReadyFenceValue=1;
   publication.Width=64;publication.Height=32;publication.BitmapBounds={0,0,64,32};publication.PublishedTick=GetTickCount64();
   if(cycle){
    check(!surface.PollPending(GetTickCount64())&&surface.LastError()==E_PENDING&&!Cancelled(*record),
     "replacement import and publication cannot make older pending metadata cancel healthy generation");
    check(!surface.Observe(predecessor,GetTickCount64())&&surface.LastError()==E_PENDING&&!Cancelled(*record)
      &&!surface.HasBase()&&!surface.PublishedTick(),"old source first observed after import is ignored without reading or renewing base");
   }
   const auto stop=GetTickCount64()+1000;bool accepted=false;
   do{accepted=surface.Observe(publication,GetTickCount64());if(!accepted)Sleep(1);}while(!accepted&&GetTickCount64()<stop);
   check(accepted&&surface.HasBase()&&surface.PublishedTick()==publication.PublishedTick,"real helper accepts copied private GPU source with unchanged source serial");
   check(!surface.Ready(),"offscreen helper remains unready without native XAML connection");
   if(cycle)check(!surface.Observe(predecessor,GetTickCount64())&&surface.LastError()==E_PENDING&&!Cancelled(*record)
     &&surface.HasBase()&&surface.PublishedTick()==publication.PublishedTick,"obsolete publication cannot clear or refresh accepted replacement base");
   predecessor=publication;
   const auto readStop=GetTickCount64()+1000;while(consumed->GetCompletedValue()<1&&GetTickCount64()<readStop)Sleep(1);
   winrt::check_bool(consumed->GetCompletedValue()>=1&&consumed->GetCompletedValue()!=UINT64_MAX);
   surface.ClearSource();check(!surface.HasBase()&&!surface.PublishedTick()&&Cancelled(*record),"ClearSource retires the consumed generation and withdraws private base");
   target=nullptr;texture=nullptr;ready=nullptr;consumed=nullptr;
   const bool retired=CompleteRetirement(*record);check(retired,"broker can acknowledge source withdrawal after final GPU read");
   if(!retired)break;
   winrt::check_bool(writer.Refresh(GetTickCount64()));
   winrt::check_bool(surface.Prepare(compositor,directory,72,4,7,64,32));
   auto* nextOffer=offered();winrt::check_bool(nextOffer!=nullptr);const auto nextGeneration=nextOffer->Offer.Generation;
   check(surface.Prepare(compositor,directory,72,4,7,64,32)&&!Cancelled(*nextOffer)&&nextOffer->Offer.Generation==nextGeneration,
    "stable Prepare preserves recovery offer without repeatedly retiring it");
  }
  surface.Reset();control.Close();writer.Withdraw();
 }catch(winrt::hresult_error const& error){printf("DEVICE ERROR: %08x\n",unsigned(error.code().value));check(false,"static source recovery fixture completes");}
 catch(...){check(false,"static source recovery fixture completes without exception");}
 std::filesystem::remove_all(directory);winrt::uninit_apartment();
}

int main(int argc,char** argv) {
 int checks=0,failed=0;
 auto check=[&](bool ok,char const* name){++checks;if(!ok){++failed;printf("FAIL: %s\n",name);}};
 GpuExpectedFrame accepted{7,20,2,2,1,100,80,{0,0,100,80},2500};
 GpuPresentationTag displayed{7,20,1,1,1000};
 check(!PresentationFresh(true,displayed,accepted,2600),"fresh new cached serial cannot renew expired old display");
 check(PresentationFresh(true,{7,20,1,1,1200},accepted,2600),"still-fresh prior output may remain while replacement is pending");
 check(!PresentationFresh(true,{7,20,2,1,2700},accepted,2600),"future display timestamp rejected");
 check(!PresentationFresh(true,{8,20,2,1,2500},accepted,2600),"wrong output epoch rejected");
 check(!PresentationFresh(true,{7,21,2,1,2500},accepted,2600),"wrong output generation rejected");
 check(!PresentationFresh(true,{7,20,3,1,2500},accepted,2600),"unaccepted newer output serial rejected");
 check(!PresentationFresh(false,{7,20,2,1,2500},accepted,2600),"disconnected native brush cannot be ready");
 check(!PresentationFresh(true,{7,20,2,1,2500},accepted,4101),"expired accepted source rejected");
 Frame frame;frame.Status=Status::Ready;frame.Revision=7;frame.Serial=2;frame.Generation=20;
 frame.ReadyFenceValue=2;frame.TextureSlot=1;frame.Width=100;frame.Height=80;frame.BitmapBounds={0,0,100,80};frame.PublishedTick=2500;frame.PayloadFlags=3;
 check(Admissible(frame,7,100,80,2600),"explicit GPU metadata admitted");
 for(unsigned payload : {0u,1u,2u,7u}) {frame.PayloadFlags=payload;check(!Admissible(frame,7,100,80,2600),"other payload rejected");}
 frame.PayloadFlags=3;frame.Pixels.push_back(0);check(!Admissible(frame,7,100,80,2600),"CPU pixels rejected");frame.Pixels.clear();
 frame.Stride=400;check(!Admissible(frame,7,100,80,2600),"GPU metadata must have zero stride");frame.Stride=0;
 frame.MotionSequence=1;check(!Admissible(frame,7,100,80,2600),"broker-rendered motion metadata rejected");frame.MotionSequence=0;
 frame.RenderedBounds.Right=100;check(!Admissible(frame,7,100,80,2600),"broker-rendered geometry rejected");frame.RenderedBounds={};
 check(!Admissible(frame,8,100,80,2600)&&!Admissible(frame,7,101,80,2600),"request epoch and allocation mismatch rejected");
 frame.PublishedTick=2700;check(!Admissible(frame,7,100,80,2600),"future source timestamp rejected");frame.PublishedTick=2500;
 frame.Error=E_FAIL;check(!Admissible(frame,7,100,80,2600),"source HRESULT rejected");frame.Error=S_OK;
 frame.Status=Status::Unavailable;check(!Admissible(frame,7,100,80,2600),"source withdrawal rejected");
 PoseQueue policy;Request request;request.Revision=7;request.Bounds={0,0,100,80};
 auto a=policy.Desired(request,100,80,{0,0,100,80},accepted);
 check(a.PoseSerial==1&&policy.NeedsDraw(a),"initial pose needs one draw");policy.Queued(a);
 check(!policy.NeedsDraw(policy.Desired(request,100,80,{0,0,100,80},accepted)),"identical pending tag does not duplicate draw");
 request.MotionSequence=22;request.HeartbeatTick=2600;
 check(policy.Desired(request,100,80,{0,0,100,80},accepted)==a,"heartbeat and motion labels do not advance unchanged field");
 auto heart=accepted;heart.PublishedTick=2600;
 check(!policy.NeedsDraw(policy.Desired(request,100,80,{0,0,100,80},heart)),"same accepted source heartbeat does not redraw");
 auto newSource=accepted;newSource.SourceSerial=3;newSource.ReadyFenceValue=3;
 auto b=policy.Desired(request,100,80,{0,0,100,80},newSource);
 check(b.PoseSerial==a.PoseSerial&&policy.NeedsDraw(b),"source advance forces rendering without fake pose advance");
 request.Bounds.Left=1;auto c=policy.Desired(request,100,80,{0,0,100,80},newSource);
 check(c.PoseSerial==a.PoseSerial+1,"geometry changes advance pose");
 auto serial=c.PoseSerial;
 request.Radius+=1;check(policy.Desired(request,100,80,{0,0,100,80},newSource).PoseSerial==++serial,"radius changes advance pose");
 request.Material.Lens+=.1;check(policy.Desired(request,100,80,{0,0,100,80},newSource).PoseSerial==++serial,"lens changes advance pose");
 request.Material.Dispersion+=.1;check(policy.Desired(request,100,80,{0,0,100,80},newSource).PoseSerial==++serial,"dispersion changes advance pose");
 request.Specular+=.1;check(policy.Desired(request,100,80,{0,0,100,80},newSource).PoseSerial==++serial,"specular changes advance pose");
 check(policy.Desired(request,100,80,{0,0,101,80},newSource).PoseSerial==++serial,"viewport changes advance pose");
 auto dips=policy.Desired(request,101,80,{0,0,101,80},newSource);check(dips.PoseSerial==++serial,"DIP size changes advance pose");
 policy.Queued(dips);policy.Clear();check(policy.NeedsDraw(dips),"presentation clearing allows same source and pose redraw");
 gpu_presenter_detail::OutputRing ring;ring.Connect();gpu_presenter_detail::Lease lease;
 check(ring.Acquire({true,true,true},0,lease)&&ring.Queue(lease,a,1),"queue first animated output");
 auto moving=a;moving.PoseSerial=5;
 check(ring.LatestCompleted(moving,1)==int(lease.Slot),"completed older pose remains eligible as animation moves");ring.Present(int(lease.Slot));
 ring.RenewPublishedTick({7,20,3,5,2600});
 check(ring.CurrentTag().PublishedTick==2500,"new source metadata cannot renew displayed old serial");
 ring.RenewPublishedTick({7,20,2,5,2600});
 check(PresentationFresh(ring.Ready(),ring.CurrentTag(),heart,4000),"validated matching heartbeat renews accepted display");
 ring.Disconnect();check(!PresentationFresh(ring.Ready(),ring.CurrentTag(),heart,2700),"disconnect withdraws readiness");
 ring.Connect();check(ring.LatestCompleted(moving,1)==int(lease.Slot),"same source reconnect can present without duplicate draw");ring.Present(int(lease.Slot));
 check(PresentationFresh(ring.Ready(),ring.CurrentTag(),heart,2700),"reconnected completed output is ready");
 GpuOffer offer{};offer.RequestRevision=7;offer.Generation=20;offer.Width=100;offer.Height=80;
 capture::gpu::input_detail::MetadataGate gate;gate.Reset(offer);
 check(gate.Observe(accepted,2600)&&gate.Commit(accepted),"private base metadata requires explicit acceptance");
 auto renewal=accepted;renewal.PublishedTick=3900;
 check(gate.Renew(renewal,4000)&&gate.Accepted().PublishedTick==3900,"validated same serial heartbeat refreshes accepted cache");
 auto wrong=renewal;++wrong.Generation;check(!gate.Observe(wrong,4000),"foreign source generation cannot renew accepted cache");
 wrong=renewal;++wrong.Revision;check(!gate.Observe(wrong,4000),"foreign revision cannot renew accepted cache");
 wrong=renewal;wrong.PublishedTick=4001;check(!gate.Renew(wrong,4000),"future heartbeat cannot renew accepted cache");
 auto pending=renewal;pending.SourceSerial=3;pending.ReadyFenceValue=3;pending.PublishedTick=4000;
 check(gate.Observe(pending,4000)&&gate.Accepted().SourceSerial==2&&gate.Accepted().PublishedTick==3900,"new pending metadata never refreshes older private base");
 check(!gate.Ready(5401),"pending metadata cannot extend accepted cache TTL");
 FakePresenter fake;PoseQueue schedule;
 auto draw=[&](GpuOutputLease const&){++fake.Draws;return S_OK;};
 auto fresh=[&]{return fake.Shown;};
 Schedule(fake,schedule,a,{{0,0,100,80},{0,0,100,80},100,80},draw,fresh);
 check(fake.Calls=="MPAQP"&&fake.Draws==1,"mapping and presentation precede acquire then GPU draw queue and repoll");
 fake.Calls.clear();Schedule(fake,schedule,a,{},draw,fresh);
 check(fake.Calls=="MPP"&&fake.Draws==1,"identical pending tag polls mapping and presentation without duplicate GPU draw");
 fake.Calls.clear();fake.Acquire=false;
 auto moved=a;++moved.PoseSerial;
 Schedule(fake,schedule,moved,{},draw,fresh);
 check(fake.Calls=="MPAP"&&fake.Draws==1,"full ring still polls presentation every frame without blocking");
 fake.Calls.clear();fake.Acquire=true;
 const auto unsupported=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
 auto reject=[&](GpuOutputLease const&){return unsupported;};
 check(Schedule(fake,schedule,moved,{},reject,fresh)==unsupported&&fake.Calls=="MPAXC","unsupported optics seal lease withdraw output and retain exact error");
 fake.Calls.clear();Schedule(fake,schedule,moved,{},draw,fresh);
 check(fake.Calls=="MPAQP"&&fake.Draws==2,"safe later pose or source can recover from rejected optics");
 fake.Calls.clear();fake.Mapping=false;
 check(Schedule(fake,schedule,moved,{},draw,fresh)==E_INVALIDARG&&fake.Calls=="MC","mapping failure clears queued identity for recovery");
 fake.Calls.clear();fake.Mapping=true;Schedule(fake,schedule,moved,{},draw,fresh);
 check(fake.Calls=="MPAQP"&&fake.Draws==3,"mapping recovery redraws formerly identical queued tag");
 fake.Calls.clear();fake.Shown=true;
 auto expired=[] {return false;};Schedule(fake,schedule,moved,{},draw,expired);
 check(fake.Calls=="MPCAQP","expired visible old serial withdrawn before new render admission");
 RingPresenter stream;PoseQueue streamedPoses;Request animated;animated.Revision=7;animated.Bounds={0,0,100,80};
 auto streamSource=accepted;int visibleCallbacks=0;uint64_t lastShown=0;
 for(uint64_t serialNumber=1;serialNumber<=12;++serialNumber){
  stream.Completed=stream.NextFence;streamSource.SourceSerial=serialNumber;streamSource.ReadyFenceValue=serialNumber;
  streamSource.PublishedTick=2500+serialNumber;++animated.Bounds.Left;
  const auto wanted=streamedPoses.Desired(animated,100,80,{0,0,100,80},streamSource);
  Schedule(stream,streamedPoses,wanted,{{0,0,100,80},animated.Bounds,100,80},[](GpuOutputLease const&){return S_OK;},
   [&]{return PresentationFresh(stream.Ready(),stream.Ring.CurrentTag(),streamSource,2600+serialNumber);});
  if(serialNumber>1){
   check(stream.Ready(),"actual scheduler presents while source and pose advance on every callback");
   if(stream.Ready()){const auto shown=stream.Ring.CurrentTag();++visibleCallbacks;
    check(shown.SourceSerial>lastShown&&shown.PublishedTick==2500+shown.SourceSerial,"streaming presentation advances with its own source timestamp");lastShown=shown.SourceSerial;}
  }
 }
 check(visibleCallbacks==11,"continuous source acceptance does not starve completed optics");
 streamSource.SourceSerial=13;streamSource.ReadyFenceValue=13;streamSource.PublishedTick=5000;stream.Completed=stream.NextFence;
 auto wanted=streamedPoses.Desired(animated,100,80,{0,0,100,80},streamSource);
 Schedule(stream,streamedPoses,wanted,{},[](GpuOutputLease const&){return S_OK;},
  [&]{return PresentationFresh(stream.Ready(),stream.Ring.CurrentTag(),streamSource,5000);});
 check(!stream.Ready(),"fresh cached source cannot revive expired older completed output during scheduling");
 stream.Completed=stream.NextFence;streamSource.SourceSerial=14;streamSource.ReadyFenceValue=14;streamSource.PublishedTick=5001;
 wanted=streamedPoses.Desired(animated,100,80,{0,0,100,80},streamSource);
 Schedule(stream,streamedPoses,wanted,{},[](GpuOutputLease const&){return S_OK;},
  [&]{return PresentationFresh(stream.Ready(),stream.Ring.CurrentTag(),streamSource,5001);});
 check(stream.Ready()&&stream.Ring.CurrentTag().SourceSerial==13,"fresh completed replacement becomes visible after expired presentation withdrawal");
 GpuLiquidSurface surface;check(!surface.Ready(2600)&&!surface.HasBase()&&surface.PublishedTick()==0,"unprepared helper has no invented source");
 surface.ClearPresentation();surface.ClearSource();surface.Reset();check(!surface.PollPending(2600),"reset helper has no pending file or GPU work");
 if(argc>1&&std::string_view(argv[1])=="--device")StaticRecovery(check);
 printf("gpu liquid surface: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
