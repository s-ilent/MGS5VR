#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include "core.hpp"
#include "stereo.hpp"

namespace mgs5vr {
using Microsoft::WRL::ComPtr;
void checkHr(HRESULT result, const char* operation);
struct TextureChannel {
    ComPtr<ID3D11Device> producerDevice;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE sharedHandle{}; // Legacy DXGI handle: owned by texture, never CloseHandle.
    D3D11_TEXTURE2D_DESC desc{};
    LUID adapterLuid{};
    uint64_t epoch{};
    FrameId published{}; // Access only while holding the DXGI keyed mutex.
    std::array<EyeFrame,2> eyes{};
};
class TextureMailbox {
public:
    // Called only on the game's immediate-context/render thread. Never waits for XR.
    bool publish(ID3D11Texture2D* source, ID3D11DeviceContext* context,EyeFrame eye={});
    bool publishStereo(const std::array<ID3D11Texture2D*,2>& sources,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes);
    std::shared_ptr<TextureChannel> latest() const;
    void invalidate();
    // Default on. When disabled, transfers run inline on the calling thread.
    void setThreadedPublish(bool on) noexcept { threadedPublish_.store(on); }
    ~TextureMailbox();
private:
    bool publishImages(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes);
    bool publishInline(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,ID3D11DeviceContext* context,const std::array<EyeFrame,2>& eyes,const std::shared_ptr<TextureChannel>& channel,FrameId frame);
    bool publishEnqueue(const std::array<ID3D11Texture2D*,2>& sources,uint32_t count,const std::shared_ptr<TextureChannel>& channel,const std::array<EyeFrame,2>& eyes,FrameId frame);
    void startWorker();
    void publishWorker();
    struct PublishJob {
        std::array<ComPtr<ID3D11Texture2D>,2> sources{};
        uint32_t count{};
        std::shared_ptr<TextureChannel> channel;
        std::array<EyeFrame,2> eyes{};
        FrameId frame{};
    };
    mutable std::mutex pointerMutex_;
    std::mutex producerMutex_;
    std::shared_ptr<TextureChannel> channel_;
    uint64_t epoch_{}, sequence_{};
    // Off-thread delivery: the present thread validates and enqueues; one worker
    // owns every keyed-mutex handshake and GPU copy, so the game's frame loop
    // never blocks on the XR consumer or on the publish transfer itself.
    std::atomic_bool threadedPublish_{};
    std::atomic_bool quit_{};
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<PublishJob> jobs_;
    std::thread worker_;
};
class TextureConsumer {
public:
    explicit TextureConsumer(ID3D11Device* device);
    // Open/copy on a separate D3D11 device; the game's context is never used here.
    bool consume(const std::shared_ptr<TextureChannel>& channel);
    ID3D11Texture2D* texture() const { return cached_.Get(); }
    FrameId frame() const { return frame_; }
    EyeFrame eye() const { return eyes_[0]; }
    std::array<EyeFrame,2> eyes() const { return eyes_; }
    void reset();
private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::shared_ptr<TextureChannel> channel_;
    ComPtr<ID3D11Texture2D> shared_, cached_;
    ComPtr<IDXGIKeyedMutex> mutex_;
    FrameId frame_{};
    std::array<EyeFrame,2> eyes_{};
};
}
