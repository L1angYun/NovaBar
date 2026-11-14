#include "../GpuTexturePresenter.h"
#include <DispatcherQueue.h>
#include <winrt/Windows.System.h>
#include <cstdio>
#include <string_view>
using namespace novabar;
using namespace novabar::gpu_presenter_detail;
int main(int argc,char** argv) {
 int checks=0,failed=0;
 auto check=[&](bool ok,char const* name){++checks;if(!ok){++failed;printf("FAIL: %s\n",name);}};
 check(!Available({S_OK,false,0,0}),"active/null compositor fence forbids writes");
 check(!Available({E_FAIL,true,1,2}),"failed compositor query forbids writes");
 check(!Available({S_OK,true,2,1}),"pending compositor release forbids writes");
 check(!Available({S_OK,true,1,UINT64_MAX}),"device-removed sentinel forbids writes");
 check(Available({S_OK,true,0,0}),"never-presented completed nonnull fence allows writes");
 check(Available({S_OK,true,7,9}),"completed compositor release allows writes");
 std::array<bool,3> free{true,true,true};
 GpuPresentationTag a{1,10,1,1,100},b{1,10,2,2,200};
 OutputRing ring;ring.Connect();Lease l0,l1,l2,l3;
 bool first=ring.Acquire(free,0,l0)&&ring.Queue(l0,a,1);
 check(first,"acquire then queue first draw");if(!first)return 1;
 check(ring.LatestCompleted(a,0)==-1,"flush or queued signal alone cannot present unfinished GPU output");
 check(ring.LatestCompleted(a,1)==int(l0.Slot),"GPU completed exact source and pose becomes presentable");
 check(ring.LatestCompleted({2,10,1,1},1)==-1,"wrong client epoch cannot present");
 check(ring.LatestCompleted({1,11,1,1},1)==-1,"wrong source generation cannot present");
 check(ring.LatestCompleted({1,10,1,2},1)==int(l0.Slot),"moving desired pose2 admits completed pose1 without starvation");
 check(ring.LatestCompleted({1,10,1,3},1)==int(l0.Slot),"continuing desired pose3 still admits completed pose1");
 check(ring.LatestCompleted({1,10,1,0},1)==-1,"future completed optics pose cannot present");
 ring.Present(int(l0.Slot));check(ring.Ready(),"connected completed presentation is ready");
 check(ring.Acquire(free,0,l1)&&l1.Slot!=l0.Slot&&ring.Queue(l1,b,2),"active output is never overwritten even with optimistic availability");
 check(ring.Acquire(free,0,l2)&&l2.Slot!=l0.Slot&&l2.Slot!=l1.Slot,"third output may be acquired while two are held");
 check(!ring.Acquire(free,0,l3),"bounded output exhaustion skips work");
 check(!ring.Queue(l2,a,3),"out of order source serial is rejected");
 check(ring.LatestCompleted(b,1)==int(l0.Slot),"newer pending source preserves completed older output identity");
 check(ring.LatestCompleted(b,2)==int(l1.Slot),"newest completed output is selected");
 ring.Present(int(l1.Slot));free[l0.Slot]=false;free[l2.Slot]=false;
 check(ring.CanRetain({1,10,3,3}),"new source pending preserves still-fresh already-visible prior source");
 ring.RenewPublishedTick({1,10,3,3,900});
 check(ring.CurrentTag().SourceSerial==2&&ring.CurrentTag().PublishedTick==200,"pending new serial cannot renew retained output source freshness");
 ring.RenewPublishedTick({1,10,2,3,250});
 check(ring.CurrentTag().PublishedTick==250,"validated exact-source heartbeat renews visible output without redraw");
 ring.RenewPublishedTick({2,10,2,3,1000});ring.RenewPublishedTick({1,11,2,3,1000});
 check(ring.CurrentTag().PublishedTick==250,"wrong epoch or generation cannot renew source heartbeat");
 check(a==GpuPresentationTag{1,10,1,1,900},"heartbeat tick does not change presentation identity");
 check(!ring.CanRetain({2,10,3,3})&&!ring.CanRetain({1,11,3,3}),"epoch or source-generation switch withdraws visible old identity");
 check(!ring.Acquire(free,2,l3),"detached output still needs compositor release");
 free[l0.Slot]=true;check(ring.Acquire(free,3,l3)&&l3.Slot==l0.Slot,"completed released old output is reusable");
 ring.Disconnect();check(!ring.Ready()&&ring.LatestCompleted(b,3)==-1,"disconnected presenter cannot be ready or present");
 ring.Connect();check(!ring.Ready()&&ring.LatestCompleted(b,3)==int(l1.Slot),"reconnect makes same serial and pose eligible for explicit re-presentation");
 ring.Present(int(l1.Slot));check(ring.Ready(),"same completed content can become ready after reconnect");
 ring.Clear();check(!ring.Ready()&&ring.LatestCompleted(b,3)==-1,"source withdrawal hides and invalidates cached presentation");
 check(!ring.Queue(l3,b,4),"clear invalidates outstanding leases");
 ring.Reset();check(!ring.Queue(l1,b,4),"resize/reset invalidates old ring generation leases");
 ring.Connect();Lease fresh;
 check(ring.Acquire({true,true,true},0,fresh)&&ring.Queue(fresh,{2,20,1,0},1),"reset permits a new source generation and restarted serial");
 OutputRing other;other.Connect();Lease independent;
 check(other.Acquire({true,true,true},0,independent)&&other.Queue(independent,{7,70,1,0},1),"independent clients own independent ring admission");
 check(other.LatestCompleted({7,70,1,0},1)==int(independent.Slot)&&ring.LatestCompleted({7,70,1,0},1)==-1,"one client's epoch cannot reveal another client's pixels");
 OutputRing victim,foreign;victim.Connect();foreign.Connect();Lease victimLease,foreignLease;
 victim.Acquire({true,true,true},0,victimLease);foreign.Acquire({true,true,true},0,foreignLease);
 check(!victim.Queue(foreignLease,{7,70,2,1},2),"separate ring identity rejects a foreign client's matching numeric lease");
 OutputRing moving;moving.Connect();Lease moving0,moving1;
 check(moving.Acquire({true,true,true},0,moving0)&&moving.Queue(moving0,{1,1,1,1},1),"moving animation queues first GPU output");
 check(moving.Acquire({true,true,true},1,moving1)&&moving1.Slot!=moving0.Slot&&moving.LatestCompleted({1,1,1,2},1)==int(moving0.Slot),"acquire prefers unused output and preserves completed content until presentation");
 check(moving.Cancel(moving1,2)&&!moving.Idle(1)&&moving.Idle(2),"canceled draw still owns its slot until private GPU fence completion");
 check(moving.Acquire({true,true,true},2,moving1),"canceled completed output becomes acquirable");
 moving.SealHeld(3);moving.Clear();check(!moving.Idle(2)&&moving.Idle(3),"clear preserves completion protection for an acquired caller draw");
 num::float3x2 checkedMapping;
 check(SUCCEEDED(ImageToLocal({{-1920,-1080,0,0},{-1800,-900,-600,-300},800,400},1280,720,checkedMapping))
   &&std::abs(checkedMapping.m11-1)<.0001&&std::abs(checkedMapping.m31+80)<.0001&&std::abs(checkedMapping.m32+120)<.0001,"negative world origin maps with explicit crop offsets");
 check(FAILED(ImageToLocal({{0,0,10,10},{0,0,10,10},std::numeric_limits<double>::quiet_NaN(),10},10,10,checkedMapping)),"nonfinite mapping size is rejected");
 // A full ring exposes a different starvation path from unused-slot admission:
 // the only completed output must survive acquiring the next animation draw.
 for(bool reconnect : {false,true}) {
  OutputRing saturated;saturated.Connect();bool setup=true;
  for(uint64_t pose=1;pose<=3;++pose){Lease lease;setup=setup&&saturated.Acquire({true,true,true},0,lease)&&saturated.Queue(lease,{1,1,1,pose},pose);}
  if(reconnect){saturated.Disconnect();saturated.Connect();}
  check(setup,"three pending outputs fill the bounded animation ring");
  for(uint64_t pose=4;pose<=12;++pose) {
   const uint64_t completed=pose-3;GpuPresentationTag desired{1,1,1,pose};
   const int before=saturated.LatestCompleted(desired,completed);Lease next;
   if(saturated.Acquire({true,true,true},completed,next))check(saturated.Queue(next,desired,pose),"saturated animation can queue an admitted draw");
   const int after=saturated.LatestCompleted(desired,completed);
   check(before<0||after>=0,"acquire preserves the sole completed candidate across nine full-ring callbacks");
   if(after>=0)saturated.Present(after);
   check(saturated.Ready(),"full-ring render-before-present eventually displays and stays ready");
  }
 }
 // The floor belongs to presentation history, independently of active binding.
 for(bool hide : {false,true})for(bool sameSource : {false,true}) {
  OutputRing history;history.Connect();Lease older,newer;
  GpuPresentationTag oldTag{1,1,1,sameSource?1ull:2ull},newTag{1,1,sameSource?1ull:2ull,2};
  bool setup=history.Acquire({true,true,true},0,older)&&history.Queue(older,oldTag,1)
   &&history.Acquire({true,true,true},0,newer)&&history.Queue(newer,newTag,2);
  history.Present(history.LatestCompleted(newTag,2));
  check(setup&&history.Ready(),"newer source or pose is presented before lifecycle transition");
  if(hide)history.Hide();else{history.Disconnect();history.Connect();}
  check(!history.Ready()&&history.LatestCompleted(oldTag,2)<0,"disconnect and hide retain the serial and pose presentation floor");
  const int same=history.LatestCompleted(newTag,2);history.Present(same);
  check(same>=0&&history.Ready()&&history.CurrentTag()==newTag,"exact last-presented tag remains eligible after reconnect or hide");
  history.Clear();Lease reset;
  check(history.Acquire({true,true,true},2,reset)&&history.Queue(reset,oldTag,3)
   &&history.LatestCompleted(oldTag,3)>=0,"explicit clear resets the presentation history floor");
 }
 // A source can advance every Rendering callback while the preceding draw
 // completes. Exact-newest selection would never display even its first frame.
 for(bool saturated : {false,true}) {
  OutputRing advancing;advancing.Connect();int presented=0;uint64_t last=0;
  if(saturated)for(uint64_t serial=1;serial<=3;++serial){Lease held;advancing.Acquire({true,true,true},0,held);advancing.Queue(held,{9,90,serial,serial,1000+serial},serial);}
  const uint64_t firstSerial=saturated?4:1;
  for(uint64_t serial=firstSerial;serial<firstSerial+12;++serial) {
   const uint64_t completed=saturated?serial-3:serial-1;
   const GpuPresentationTag desired{9,90,serial,serial,1000+serial};
   const int selected=advancing.LatestCompleted(desired,completed);
   if(completed) {
    check(selected>=0,"continuously advancing source and pose admits prior completed output");
    if(selected>=0){advancing.Present(selected);auto shown=advancing.CurrentTag();
     check(shown.SourceSerial>=last&&shown.SourceSerial<=completed,"source presentation never regresses under source churn");
     last=shown.SourceSerial;++presented;advancing.RenewPublishedTick(desired);
     check(advancing.CurrentTag().PublishedTick==1000+shown.SourceSerial,"new desired source never renews prior displayed source tick");
    }
   }
   Lease target;if(advancing.Acquire({true,true,true},completed,target))
    check(advancing.Queue(target,desired,serial),"continuous source draw obeys queue monotonicity");
   const int after=advancing.LatestCompleted(desired,completed);
   check(!completed||after>=0,"ring admission cannot lose last eligible completed source");
  }
  check(presented>=10&&last>=8,"moving source and pose keep progressing through bounded ring pressure");
 }
 OutputRing timestamps;timestamps.Connect();Lease expired,freshOutput;
 check(timestamps.Acquire({true,true,true},0,expired)&&timestamps.Queue(expired,{3,30,1,1,1000},1)
  &&timestamps.Acquire({true,true,true},0,freshOutput)&&timestamps.Queue(freshOutput,{3,30,2,2,2500},2),"seed expired and fresh completed outputs");
 check(timestamps.LatestCompleted({3,30,3,3,2600},2)==int(freshOutput.Slot),"expired older output cannot mask a newer fresh completed output");
 timestamps.Present(int(freshOutput.Slot));
 check(timestamps.LatestCompleted({3,30,1,3,2600},2)<0,"broader eligibility preserves monotonic source floor");
 if(argc>1&&std::string_view(argv[1])=="--device") {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  try {
   DispatcherQueueOptions options{sizeof(options),DQTYPE_THREAD_CURRENT,DQTAT_COM_STA};winrt::Windows::System::DispatcherQueueController queue{nullptr};
   winrt::check_hresult(CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
   winrt::Windows::UI::Composition::Compositor compositor;
   winrt::com_ptr<ID3D11Device> device;winrt::com_ptr<ID3D11DeviceContext> context;
   winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
   GpuTexturePresenter presenter;
   check(!presenter.Initialize(compositor,nullptr,context.get())&&!presenter.Ready(),"missing caller device fails closed");
   winrt::com_ptr<ID3D11DeviceContext> deferred;winrt::check_hresult(device->CreateDeferredContext(0,deferred.put()));
   check(!presenter.Initialize(compositor,device.get(),deferred.get()),"deferred context cannot signal output-ready fence");
   check(presenter.Initialize(compositor,device.get(),context.get())&&presenter.EnsureSize(64,32),"caller device creates bounded raw composition outputs headlessly");
   check(!presenter.Ready(),"native XAML connection is required for ready");
   GpuOutputLease output;
   check(presenter.TryAcquireTarget(output)&&output.Target,"available raw target can be acquired");
   if(output.Target) {
    winrt::com_ptr<ID3D11Resource> resource;output.Target->GetResource(resource.put());auto texture=resource.as<ID3D11Texture2D>();D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    winrt::com_ptr<ID3D11Device> owner;resource->GetDevice(owner.put());
    check(owner==device&&desc.Width==64&&desc.Height==32&&desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM&&desc.MiscFlags==0x802&&desc.BindFlags==(D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE),"output is caller-device BGRA RT/SRV shared NT texture");
    const float color[]{.2f,.3f,.4f,1};context->ClearRenderTargetView(output.Target,color);
    check(presenter.QueueDraw(output,{1,1,1,1}),"queue protects actual draw with private GPU completion fence");
   }
   check(!presenter.TryPresent({1,1,1,2})&&!presenter.Ready(),"headless disconnected raw brush never becomes ready");
   check(presenter.UpdateMapping({{0,0,96,48},{12,6,84,42},48,24}),"mapping updates independently of GPU draw");
   auto matrix=presenter.SurfaceBrush().TransformMatrix();
   check(std::abs(matrix.m11-1.f)<.0001f&&std::abs(matrix.m22-1.f)<.0001f&&std::abs(matrix.m31+8.f)<.0001f&&std::abs(matrix.m32+4.f)<.0001f,"physical canvas maps to current local DIPs");
   check(presenter.UpdateMapping({{0,0,96,48},{24,12,96,48},48,24})&&std::abs(presenter.SurfaceBrush().TransformMatrix().m31+16.f)<.0001f,"new animation mapping applies without new texture");
   GpuOutputLease canceled;check(presenter.TryAcquireTarget(canceled)&&presenter.CancelDraw(canceled),"caller can abandon a target while preserving GPU ownership");
   const uint64_t resizeStop=GetTickCount64()+500;bool resized=false;
   do{resized=presenter.EnsureSize(32,16);if(!resized&&presenter.LastError()==E_PENDING)Sleep(1);else break;}while(GetTickCount64()<resizeStop);
   check(resized&&!presenter.Ready(),"resize retires completed outputs without blocking or retaining old readiness");
   check(!presenter.QueueDraw(output,{1,1,1,2}),"resize rejects leases from the preceding output generation");
   auto old=presenter.SurfaceBrush();presenter.Clear();
   check(!presenter.Ready()&&!old.Surface(),"clear withdraws shared surface immediately");
   check(!presenter.UpdateMapping({{0,0,96,48},{0,0,0,48},48,24})&&!presenter.Ready()&&!old.Surface(),"invalid mapping fails transparent");
   check(!presenter.EnsureSize(0,32)&&!presenter.Ready(),"invalid resize fails transparent");
   presenter.Reset();check(!presenter.Ready()&&!old.Surface(),"reset leaves externally retained native brush transparent");
   check(!presenter.QueueDraw(output,{1,1,1,2}),"reset rejects resource lease from previous generation");
  }catch(...){++failed;printf("DEVICE ERROR: %08x\n",unsigned(winrt::to_hresult()));}
  winrt::uninit_apartment();
 }
 printf("GPU texture presenter: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
