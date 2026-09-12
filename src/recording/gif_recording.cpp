#include "recording/gif_recording.h"
#include "recording/recording_timeline.h"
#include "shared/platform/capture_platform.h"
#include <wincodec.h>
#include <winrt/base.h>
#include <thread>
#include <chrono>

namespace capture {
namespace {
UINT CaptureDpi(const RecordingOptions& options) {
    if(options.gifDpi)return std::max(96u,options.gifDpi);
    UINT x=96,y=96;
    HMODULE module=LoadLibraryExW(L"Shcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(module) {
        using Query=HRESULT(WINAPI*)(HMONITOR,int,UINT*,UINT*);
        auto query=reinterpret_cast<Query>(GetProcAddress(module,"GetDpiForMonitor"));
        if(!query || FAILED(query(MonitorFromRect(&options.region,MONITOR_DEFAULTTONEAREST),0,&x,&y)))x=96;
        FreeLibrary(module);
    }
    return std::max(96u,x);
}
void Metadata(IWICMetadataQueryWriter* writer,const wchar_t* name,USHORT value) {
    PROPVARIANT v{}; v.vt=VT_UI2; v.uiVal=value; winrt::check_hresult(writer->SetMetadataByName(name,&v));
}
class GifWriter {
    UINT dpi_;
    winrt::com_ptr<IWICImagingFactory> factory_=winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    winrt::com_ptr<IWICStream> stream_;
    winrt::com_ptr<IWICBitmapEncoder> encoder_;
public:
    explicit GifWriter(const std::wstring& path,UINT dpi):dpi_(dpi) {
        winrt::check_hresult(factory_->CreateStream(stream_.put()));
        winrt::check_hresult(stream_->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
        winrt::check_hresult(factory_->CreateEncoder(GUID_ContainerFormatGif,nullptr,encoder_.put()));
        winrt::check_hresult(encoder_->Initialize(stream_.get(),WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICMetadataQueryWriter> metadata;
        winrt::check_hresult(encoder_->GetMetadataQueryWriter(metadata.put()));
        BYTE application[]={'N','E','T','S','C','A','P','E','2','.','0'},data[]={3,1,0,0,0};
        PROPVARIANT v{}; v.vt=VT_VECTOR|VT_UI1; v.caub={sizeof(application),application};
        winrt::check_hresult(metadata->SetMetadataByName(L"/appext/application",&v));
        v.caub={sizeof(data),data}; winrt::check_hresult(metadata->SetMetadataByName(L"/appext/data",&v));
    }
    void Frame(const Image& image,int64_t delay) {
        // One pending source frame and one quantized frame bound memory regardless
        // of recording duration. Long holds are split at GIF's 16-bit delay limit.
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(factory_->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppBGRA,
            image.width*4,UINT(image.pixels.size()*4),reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.pixels.data())),bitmap.put()));
        const UINT width=std::max(1,MulDiv(image.width,96,int(dpi_))),height=std::max(1,MulDiv(image.height,96,int(dpi_)));
        winrt::com_ptr<IWICBitmapScaler> scaled;
        IWICBitmapSource* source=bitmap.get();
        if(width!=UINT(image.width)||height!=UINT(image.height)) {
            winrt::check_hresult(factory_->CreateBitmapScaler(scaled.put()));
            // Scale the already composed image, including annotations and cursor.
            winrt::check_hresult(scaled->Initialize(source,width,height,WICBitmapInterpolationModeFant));source=scaled.get();
        }
        winrt::com_ptr<IWICPalette> palette; winrt::check_hresult(factory_->CreatePalette(palette.put()));
        winrt::check_hresult(palette->InitializeFromBitmap(source,256,FALSE));
        winrt::com_ptr<IWICFormatConverter> indexed; winrt::check_hresult(factory_->CreateFormatConverter(indexed.put()));
        winrt::check_hresult(indexed->Initialize(source,GUID_WICPixelFormat8bppIndexed,WICBitmapDitherTypeNone,palette.get(),0,WICBitmapPaletteTypeCustom));
        do {
            const USHORT part=USHORT(std::clamp<int64_t>(delay,1,65535));
            winrt::com_ptr<IWICBitmapFrameEncode> frame; winrt::check_hresult(encoder_->CreateNewFrame(frame.put(),nullptr));
            winrt::check_hresult(frame->Initialize(nullptr)); winrt::check_hresult(frame->SetSize(width,height));
            WICPixelFormatGUID format=GUID_WICPixelFormat8bppIndexed; winrt::check_hresult(frame->SetPixelFormat(&format));
            winrt::check_hresult(frame->SetPalette(palette.get()));
            winrt::com_ptr<IWICMetadataQueryWriter> metadata; winrt::check_hresult(frame->GetMetadataQueryWriter(metadata.put()));
            Metadata(metadata.get(),L"/grctlext/Delay",part);
            PROPVARIANT disposal{}; disposal.vt=VT_UI1; disposal.bVal=1;
            winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Disposal",&disposal));
            winrt::check_hresult(frame->WriteSource(indexed.get(),nullptr)); winrt::check_hresult(frame->Commit());
            delay-=part;
        } while(delay>0);
    }
    void Finish() { winrt::check_hresult(encoder_->Commit()); encoder_=nullptr; stream_=nullptr; }
};
}
void RecordGif(const RecordingOptions& options,RecordingState& state) {
    bool apartment=false,frames=false; std::wstring error,saved;
    const auto temporary=options.path+L".partial.gif";
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment=true;
        if(options.frameRate<1 || options.frameRate>30) throw std::invalid_argument("GIF frame rate must be 1..30");
        BorderlessCapture capture(options.region,false);
        HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) winrt::throw_last_error(); CloseHandle(file);
        { std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles.push_back(temporary); }
        GifWriter writer(temporary,CaptureDpi(options)); state.ready=true;
        while(options.waitForStart && !state.start && !state.stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Timeline timeline; timeline.Start(ClockNow()); bool paused=false;
        AnnotationRenderer renderer; Image desktop,pending;
        int64_t next=0,writtenCs=0,lastFresh=ClockNow();
        while(!state.stop) {
            const auto now=ClockNow(); const bool pause=state.paused;
            if(pause!=paused) { if(pause) timeline.Pause(now); else timeline.Resume(now); paused=pause; }
            const auto elapsed=timeline.Elapsed(now); state.elapsed=elapsed;
            if(!paused && elapsed>=next) {
                Image fresh; if(capture.Next(fresh)) { desktop=std::move(fresh); lastFresh=now; }
                if(now-lastFresh>100000000) throw std::runtime_error("Screen capture returned no frames for 10 seconds");
                if(!desktop.Empty()) {
                    // Delay is derived from the actual capture timeline, so slow
                    // quantization drops samples rather than speeding up playback.
                    const int64_t endCs=(elapsed+50000)/100000;
                    if(!pending.Empty() && endCs>writtenCs) { writer.Frame(pending,endCs-writtenCs); writtenCs=endCs; frames=true; }
                    pending=desktop;
                    std::shared_ptr<const RecordingAnnotations> snapshot; bool laserMode; LaserPointer laser;
                    { std::lock_guard<std::mutex> lock(state.mutex); snapshot=state.annotations; laserMode=state.laserMode; laser=state.liveLaser; }
                    const RecordingAnnotations empty;
                    const bool cursor=state.cursorEnabled;
                    const LaserPointer hidden{};
                    CompositeRecording(pending,snapshot?*snapshot:empty,renderer,now,cursor&&laserMode?&laser:&hidden);
                    if(cursor && !laserMode) capture.DrawCursor(pending);
                    next=(elapsed*options.frameRate/10000000+1)*10000000/options.frameRate;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(!pending.Empty() && state.stopReason!=RecordingStopReason::Discard) {
            const auto endCs=(timeline.Elapsed(ClockNow())+50000)/100000;
            writer.Frame(pending,std::max<int64_t>(1,endCs-writtenCs)); frames=true;
        }
        if(frames && state.stopReason!=RecordingStopReason::Discard) { writer.Finish(); saved=options.path; }
    } catch(...) { error=CurrentError(L"GIF 录制失败"); }
    if(!saved.empty() && error.empty()) {
        if(!MoveFileExW(temporary.c_str(),saved.c_str(),MOVEFILE_WRITE_THROUGH)) { saved.clear(); error=ErrorMessage(L"GIF 文件封装失败",HRESULT_FROM_WIN32(GetLastError())); }
        else { std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles={saved}; }
    } else if(error.empty() && state.stopReason!=RecordingStopReason::Discard) {
        if(!DeleteFileW(temporary.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND) error=ErrorMessage(L"删除空 GIF 缓存失败",HRESULT_FROM_WIN32(GetLastError()));
        else { std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles.clear(); }
    }
    if(apartment) winrt::uninit_apartment();
    { std::lock_guard<std::mutex> lock(state.mutex); state.savedPath=std::move(saved); state.error=std::move(error); }
    state.finished=true;
}
}
