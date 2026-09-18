#include "mgs5vr/mailbox.hpp"
#include <stdexcept>
#include <sstream>
#include <chrono>
#include "mgs5vr/log.hpp"

namespace mgs5vr {
void checkHr(HRESULT r,const char* operation) {
    if(FAILED(r)) { std::ostringstream s; s<<operation<<" HRESULT=0x"<<std::hex<<static_cast<unsigned long>(r); throw std::runtime_error(s.str()); }
}
namespace {
using CaptureClock=std::chrono::steady_clock;
double elapsed(CaptureClock::time_point a,CaptureClock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
void reportTransfer(bool producer,const std::array<double,4>& sample) noexcept {try{
    struct Stats {CaptureClock::time_point since=CaptureClock::now();std::array<double,4> total{},maximum{};uint64_t count{};};
    thread_local std::array<Stats,2> stats;auto& s=stats[producer?0:1];++s.count;
    for(size_t i=0;i<sample.size();++i){s.total[i]+=sample[i];s.maximum[i]=std::max(s.maximum[i],sample[i]);}
    const auto now=CaptureClock::now();if(now-s.since<std::chrono::seconds(5))return;
    std::ostringstream line;line<<"Mailbox "<<(producer?"producer":"consumer")<<" ms acquire_queue_flush_release=";
    for(size_t i=0;i<sample.size();++i){if(i)line<<';';line<<s.total[i]/static_cast<double>(s.count)<<','<<s.maximum[i];}
    log(line.str());s={};
}catch(...){} }
class KeyRelease {
public:
    KeyRelease(IDXGIKeyedMutex* m,UINT64 key,ID3D11DeviceContext* c,double acquireMs):mutex(m),releaseKey(key),context(c),acquireTime(acquireMs){}
    ~KeyRelease() {const auto copied=CaptureClock::now();context->Flush();const auto flushed=CaptureClock::now();mutex->ReleaseSync(releaseKey);
        reportTransfer(releaseKey==1,{acquireTime,elapsed(start,copied),elapsed(copied,flushed),elapsed(flushed,CaptureClock::now())});}
    KeyRelease(const KeyRelease&)=delete;
    KeyRelease& operator=(const KeyRelease&)=delete;
private:
    IDXGIKeyedMutex* mutex;
    UINT64 releaseKey;
    ID3D11DeviceContext* context;
    double acquireTime{};
    CaptureClock::time_point start=CaptureClock::now();
};
bool acquire(IDXGIKeyedMutex* m,UINT64 key) {
    const auto r=m->AcquireSync(key,0);
    if(r==static_cast<HRESULT>(WAIT_TIMEOUT)||r==DXGI_ERROR_WAIT_TIMEOUT) return false;
    // WAIT_ABANDONED is a success-severity HRESULT but an invalid resource.
    if(r!=S_OK) throw std::runtime_error("DXGI keyed mutex unavailable/abandoned: "+std::to_string(r));
    return true;
}
bool supported(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        ||f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
}
std::shared_ptr<TextureChannel> TextureMailbox::latest() const { std::lock_guard lock(pointerMutex_); return channel_; }
void TextureMailbox::invalidate() {
    {std::lock_guard jobs(jobMutex_);jobs_.clear();}
    std::lock_guard producer(producerMutex_);
    std::lock_guard pointer(pointerMutex_);
    channel_.reset();
}
TextureMailbox::~TextureMailbox() {
    quit_.store(true);
    jobCv_.notify_all();
    if(worker_.joinable())worker_.join();
}
bool TextureMailbox::publish(ID3D11Texture2D* source,ID3D11DeviceContext* context,EyeFrame eye) {
    return publishImages({source,nullptr},1,context,{eye,{}});
}
bool TextureMailbox::publishStereo(const std::array<ID3D11Texture2D*,2>& sources,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes){
    if(!eyes[0].sourceSequence||eyes[0].sourceSequence!=eyes[1].sourceSequence||eyes[0].trackingSequence!=eyes[1].trackingSequence)
        throw std::invalid_argument("Stereo images must share one native scene transaction");
    return publishImages(sources,2,context,eyes);
}
bool TextureMailbox::publishImages(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes) {
    auto* source=sources[0];
    if(!source||!context) throw std::invalid_argument("missing D3D11 source/context");
    std::lock_guard producer(producerMutex_);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if(!supported(desc.Format)||!desc.Width||!desc.Height||desc.ArraySize!=1||desc.MipLevels!=1)
        throw std::runtime_error("unsupported capture format/shape; only single RGBA8/BGRA8 textures are supported");
    ComPtr<ID3D11Device> device, contextDevice;
    source->GetDevice(&device); context->GetDevice(&contextDevice);
    if(device.Get()!=contextDevice.Get()) throw std::runtime_error("capture texture/context device mismatch");
    if(count==2){
        if(!sources[1])throw std::invalid_argument("Missing right native eye image");
        D3D11_TEXTURE2D_DESC right{};sources[1]->GetDesc(&right);ComPtr<ID3D11Device> rightDevice;sources[1]->GetDevice(&rightDevice);
        if(right.Width!=desc.Width||right.Height!=desc.Height||right.Format!=desc.Format||right.ArraySize!=1||right.MipLevels!=1
            ||right.SampleDesc.Count!=desc.SampleDesc.Count||rightDevice.Get()!=device.Get())throw std::invalid_argument("Native eye image mismatch");
    }
    auto channel=latest();
    if(!channel||channel->producerDevice.Get()!=device.Get()||channel->desc.Width!=desc.Width
        ||channel->desc.Height!=desc.Height||channel->desc.Format!=desc.Format||channel->desc.ArraySize!=count) {
        channel=std::make_shared<TextureChannel>();
        channel->producerDevice=device;
        channel->desc=desc;
        channel->desc.SampleDesc={1,0};
        channel->desc.ArraySize=count;
        channel->desc.Usage=D3D11_USAGE_DEFAULT;
        channel->desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        channel->desc.CPUAccessFlags=0;
        channel->desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        checkHr(device->CreateTexture2D(&channel->desc,nullptr,&channel->texture),"Create mailbox texture");
        checkHr(channel->texture.As(&channel->mutex),"Query producer keyed mutex");
        ComPtr<IDXGIResource> resource;
        checkHr(channel->texture.As(&resource),"Query shared resource");
        checkHr(resource->GetSharedHandle(&channel->sharedHandle),"Get shared handle");
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC adapterDesc{};
        checkHr(device.As(&dxgi),"Query producer DXGI device");
        checkHr(dxgi->GetAdapter(&adapter),"Get producer adapter");
        checkHr(adapter->GetDesc(&adapterDesc),"Get producer adapter description");
        channel->adapterLuid=adapterDesc.AdapterLuid;
        channel->epoch=++epoch_;
        std::lock_guard pointer(pointerMutex_); channel_=channel;
    }
    const FrameId frame{channel->epoch,++sequence_};
    if(threadedPublish_.load())return publishEnqueue(sources,count,channel,eyes,frame);
    return publishInline(sources,count,context,eyes,channel,frame);
}
bool TextureMailbox::publishInline(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes,const std::shared_ptr<TextureChannel>& channel,FrameId frame){
    D3D11_TEXTURE2D_DESC desc{};sources[0]->GetDesc(&desc);
    const auto acquiring=CaptureClock::now();if(!acquire(channel->mutex.Get(),0)) return false;
    KeyRelease release(channel->mutex.Get(),1,context,elapsed(acquiring,CaptureClock::now()));
    for(uint32_t n=0;n<count;++n){
        if(desc.SampleDesc.Count>1)context->ResolveSubresource(channel->texture.Get(),n,sources[n],0,desc.Format);
        else context->CopySubresourceRegion(channel->texture.Get(),n,0,0,0,sources[n],0,nullptr);
    }
    checkHr(channel->producerDevice->GetDeviceRemovedReason(),"Producer device health");
    channel->published=frame;
    channel->eyes=eyes;
    return true;
}
bool TextureMailbox::publishEnqueue(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,const std::shared_ptr<TextureChannel>& channel,const std::array<EyeFrame,2>& eyes,FrameId frame){
    startWorker();
    {
        std::lock_guard lock(jobMutex_);
        // Newest wins when the worker falls behind a burst, mirroring the
        // single-slot mailbox the consumer reads: the freshest completed pair
        // is the only one the XR thread would display anyway.
        while(jobs_.size()>=3)jobs_.pop_front();
        auto& job=jobs_.emplace_back();
        for(uint32_t n=0;n<count;++n)job.sources[n]=sources[n];
        job.count=count;job.channel=channel;job.eyes=eyes;job.frame=frame;
    }
    jobCv_.notify_one();
    return true;
}
void TextureMailbox::startWorker(){
    if(worker_.joinable())return;
    try{
        worker_=std::thread(&TextureMailbox::publishWorker,this);
    }catch(...){
        // Without a worker every transfer stays on the calling thread.
        threadedPublish_.store(false);
    }
}
void TextureMailbox::publishWorker(){
    ComPtr<ID3D11DeviceContext> deferred,immediate;
    ComPtr<ID3D11Device> deviceSeen;
    for(;;){
        PublishJob work;
        {
            std::unique_lock lock(jobMutex_);
            jobCv_.wait(lock,[&]{return !jobs_.empty()||quit_.load();});
            if(quit_.load())return;
            work=std::move(jobs_.front());
            jobs_.pop_front();
        }
        const auto channel=work.channel;
        if(!channel||!work.sources[0]||!work.count)continue;
        try{
            const auto device=channel->producerDevice;
            if(!deferred||deviceSeen.Get()!=device.Get()){
                ComPtr<ID3D11DeviceContext> created;
                checkHr(device->CreateDeferredContext(0,&created),"Create mailbox publish context");
                device->GetImmediateContext(&immediate);
                deferred=created;
                deviceSeen=device;
                // Command lists run on the immediate context from this worker
                // while the game renders. DXVK serializes immediate-context
                // access internally, and native engines using deferred
                // contexts typically enable D3D11.1 multithread protection
                // themselves (ID3D11Multithread, gated behind NTDDI in the
                // SDK, so it is not queried here).
            }
        }catch(...){
            // A single-threaded device rejects deferred contexts. Fall back to
            // inline transfers permanently instead of failing every publish.
            threadedPublish_.store(false);
            static std::atomic_bool reported{};
            if(!reported.exchange(true))log("Mailbox publish worker disabled; transfers remain on the present thread");
            continue;
        }
        const auto pickup=CaptureClock::now();
        bool acquired{};
        try{
            // The consumer owns the mutex between its own snapshots. Retry
            // instead of dropping the pair; a newer pair replaces this job when
            // one is waiting, otherwise waiting preserves it.
            while(!(acquired=acquire(channel->mutex.Get(),0))){
                bool replaced{};
                {std::lock_guard lock(jobMutex_);replaced=!jobs_.empty()||quit_.load();}
                if(replaced)break;
                Sleep(1);
            }
            if(!acquired)continue;
            KeyRelease release(channel->mutex.Get(),1,immediate.Get(),elapsed(pickup,CaptureClock::now()));
            // No ClearState here: it would be recorded into the command list
            // and executed against the immediate context, wiping the game's
            // bindings. FinishCommandList(FALSE) already resets the recording
            // context between publishes, and plain copies need no state.
            for(uint32_t n=0;n<work.count;++n){
                D3D11_TEXTURE2D_DESC sample{};work.sources[n]->GetDesc(&sample);
                if(sample.SampleDesc.Count>1)deferred->ResolveSubresource(channel->texture.Get(),n,work.sources[n].Get(),0,sample.Format);
                else deferred->CopySubresourceRegion(channel->texture.Get(),n,0,0,0,work.sources[n].Get(),0,nullptr);
            }
            ComPtr<ID3D11CommandList> list;
            checkHr(deferred->FinishCommandList(FALSE,&list),"Record mailbox publish");
            // D3D11 executes a single command list on the immediate context;
            // the plural D3D12-style overload does not exist on it.
            immediate->ExecuteCommandList(list.Get(),FALSE);
            checkHr(deviceSeen->GetDeviceRemovedReason(),"Producer device health");
            // Metadata lands before the keyed handoff; the consumer reads it
            // only while holding the mutex, exactly as with inline transfers.
            channel->published=work.frame;
            channel->eyes=work.eyes;
        }catch(const std::exception& error){
            // Never strand the keyed mutex: hand the key back even for a failed
            // transfer so the consumer keeps ticking.
            if(acquired)channel->mutex->ReleaseSync(1);
            static std::atomic_bool reported{};
            if(!reported.exchange(true))log(std::string("Mailbox publish worker retrying after: ")+error.what());
        }catch(...){
            if(acquired)channel->mutex->ReleaseSync(1);
            static std::atomic_bool reported{};
            if(!reported.exchange(true))log("Mailbox publish worker retrying after an unexpected exception");
        }
    }
}
TextureConsumer::TextureConsumer(ID3D11Device* device):device_(device) {
    if(!device) throw std::invalid_argument("missing consumer device");
    device_->GetImmediateContext(&context_);
}
void TextureConsumer::reset() { cached_.Reset(); shared_.Reset(); mutex_.Reset(); channel_.reset(); frame_={};eyes_={}; }
bool TextureConsumer::consume(const std::shared_ptr<TextureChannel>& channel) {
    if(!channel) return false;
    if(channel_!=channel) {
        reset();
        checkHr(device_->OpenSharedResource(channel->sharedHandle,IID_PPV_ARGS(&shared_)),"Open mailbox on XR device");
        checkHr(shared_.As(&mutex_),"Query consumer keyed mutex");
        auto desc=channel->desc; desc.MiscFlags=0;
        checkHr(device_->CreateTexture2D(&desc,nullptr,&cached_),"Create consumer cache");
        channel_=channel;
    }
    const auto acquiring=CaptureClock::now();if(!acquire(mutex_.Get(),1)) return false;
    KeyRelease release(mutex_.Get(),0,context_.Get(),elapsed(acquiring,CaptureClock::now()));
    // The producer has not completed a newer scene transaction since the last
    // snapshot. Repeating the full-texture copy would spend the same GPU
    // bandwidth on identical pixels in every XR tick between publications and
    // deepen the command queue behind the game's own stereo rendering.
    if(channel->published==frame_) return false;
    context_->CopyResource(cached_.Get(),shared_.Get());
    checkHr(device_->GetDeviceRemovedReason(),"Consumer device health");
    frame_=channel->published;
    eyes_=channel->eyes;
    return true;
}
}
