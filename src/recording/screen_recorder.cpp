#include "recording/recording_timeline.h"
#include "recording/screen_recorder.h"
#include "recording/gif_recording.h"
#include "shared/platform/capture_platform.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/base.h>
#include <deque>
#include <thread>
#include <chrono>
#include <cstring>

namespace capture {
using namespace winrt;
std::wstring DefaultAudioDevice(bool microphone) {
    auto enumerator=create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    com_ptr<IMMDevice> device;
    if(FAILED(enumerator->GetDefaultAudioEndpoint(microphone?eCapture:eRender,eConsole,device.put()))) return L"unavailable";
    LPWSTR id=nullptr; check_hresult(device->GetId(&id));
    std::wstring result=id; CoTaskMemFree(id); return result;
}struct AudioPacket { int64_t start{}; std::vector<float> samples; };
class AudioSource {
public:
    AudioSource(bool mic,const std::wstring& id) {
        auto enumerator=create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        com_ptr<IMMDevice> device;
        if(id.empty()) check_hresult(enumerator->GetDefaultAudioEndpoint(mic?eCapture:eRender,eConsole,device.put()));
        else check_hresult(enumerator->GetDevice(id.c_str(),device.put()));
        check_hresult(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,client_.put_void()));
        WAVEFORMATEX format{}; format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT; format.nChannels=2;
        format.nSamplesPerSec=48000; format.wBitsPerSample=32; format.nBlockAlign=8; format.nAvgBytesPerSec=384000;
        const DWORD flags=AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY|(mic?0:AUDCLNT_STREAMFLAGS_LOOPBACK);
        check_hresult(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,1000000,0,&format,nullptr));
        check_hresult(client_->GetService(__uuidof(IAudioCaptureClient),capture_.put_void()));
        check_hresult(client_->Start());
    }
    ~AudioSource() { if(client_) client_->Stop(); }
    void Poll(const Timeline& timeline,bool discard,int64_t cutoff) {
        UINT next=0; check_hresult(capture_->GetNextPacketSize(&next));
        while(next) {
            BYTE* bytes=nullptr; UINT count=0; DWORD flags=0; UINT64 devicePosition=0,qpc=0;
            check_hresult(capture_->GetBuffer(&bytes,&count,&flags,&devicePosition,&qpc));
            try {
                if(!discard && int64_t(qpc)>=cutoff && !(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                    AudioPacket p; p.start=timeline.Elapsed(int64_t(qpc))*48000/10000000;
                    p.samples.resize(size_t(count)*2,0);
                    if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT)) std::memcpy(p.samples.data(),bytes,p.samples.size()*4);
                    packets_.push_back(std::move(p));
                    if(packets_.back().start-packets_.front().start>96000)
                        throw hresult_error(E_ABORT,L"音频编码积压超过两秒，录制已停止。");
                }
            } catch(...) { capture_->ReleaseBuffer(count); throw; }
            check_hresult(capture_->ReleaseBuffer(count)); check_hresult(capture_->GetNextPacketSize(&next));
        }
        if(discard) packets_.clear();
    }
    void Mix(int64_t start,int count,std::vector<float>& mix) {
        while(!packets_.empty() && packets_.front().start+int64_t(packets_.front().samples.size()/2)<=start) packets_.pop_front();
        for(const auto& packet:packets_) {
            const int64_t begin=std::max(start,packet.start),end=std::min(start+count,packet.start+int64_t(packet.samples.size()/2));
            for(int64_t t=begin;t<end;++t) for(int c=0;c<2;++c) {
                auto& v=mix[size_t(t-start)*2+c];
                v=MixSample(v,packet.samples[size_t(t-packet.start)*2+c]);
            }
        }
    }
private:
    com_ptr<IAudioClient> client_;
    com_ptr<IAudioCaptureClient> capture_;
    std::deque<AudioPacket> packets_;
};
class MovieWriter {
public:
    MovieWriter(const std::wstring& path,int w,int h,bool audio) : width_((w+1)&~1),height_((h+1)&~1) {
        com_ptr<IMFAttributes> attributes; check_hresult(MFCreateAttributes(attributes.put(),3));
        check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE));
        check_hresult(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE));
        check_hresult(MFCreateSinkWriterFromURL(path.c_str(),nullptr,attributes.get(),writer_.put()));
        com_ptr<IMFMediaType> output; check_hresult(MFCreateMediaType(output.put()));
        check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
        check_hresult(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264));
        check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE,UINT32(std::clamp<int64_t>(int64_t(w)*h*4,2000000,40000000))));
        check_hresult(output->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
        check_hresult(MFSetAttributeSize(output.get(),MF_MT_FRAME_SIZE,width_,height_));
        check_hresult(MFSetAttributeRatio(output.get(),MF_MT_FRAME_RATE,30,1));
        check_hresult(MFSetAttributeRatio(output.get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));
        check_hresult(writer_->AddStream(output.get(),&video_));
        com_ptr<IMFMediaType> input; check_hresult(MFCreateMediaType(input.put()));
        check_hresult(input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
        check_hresult(input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32));
        check_hresult(input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
        check_hresult(MFSetAttributeSize(input.get(),MF_MT_FRAME_SIZE,width_,height_));
        check_hresult(MFSetAttributeRatio(input.get(),MF_MT_FRAME_RATE,30,1));
        check_hresult(MFSetAttributeRatio(input.get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));
        check_hresult(input->SetUINT32(MF_MT_DEFAULT_STRIDE,width_*4));
        check_hresult(writer_->SetInputMediaType(video_,input.get(),nullptr));
        if(audio) {
            com_ptr<IMFMediaType> a; check_hresult(MFCreateMediaType(a.put()));
            check_hresult(a->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));
            check_hresult(a->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC));
            check_hresult(a->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));
            check_hresult(a->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));
            check_hresult(a->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
            check_hresult(a->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,24000));
            check_hresult(a->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE,0));
            check_hresult(a->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,0x29));
            check_hresult(writer_->AddStream(a.get(),&audio_));
            com_ptr<IMFMediaType> pcm; check_hresult(MFCreateMediaType(pcm.put()));
            check_hresult(pcm->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio));
            check_hresult(pcm->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM));
            check_hresult(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2));
            check_hresult(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000));
            check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16));
            check_hresult(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4));
            check_hresult(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,192000));
            check_hresult(writer_->SetInputMediaType(audio_,pcm.get(),nullptr));
        }
        check_hresult(writer_->BeginWriting());
    }
    void Video(const Image& image,int64_t time,int64_t duration) {
        std::vector<uint32_t> pixels(size_t(width_)*height_,0xff000000);
        for(int y=0;y<image.height;++y) std::copy_n(image.pixels.data()+size_t(y)*image.width,image.width,pixels.data()+size_t(y)*width_);
        Sample(video_,pixels.data(),DWORD(pixels.size()*4),time,duration);
    }
    void Audio(const std::vector<float>& data,int64_t start) {
        std::vector<int16_t> pcm(data.size());
        std::transform(data.begin(),data.end(),pcm.begin(),[](float v){return int16_t(std::lround(std::clamp(v,-1.0f,1.0f)*32767));});
        Sample(audio_,pcm.data(),DWORD(pcm.size()*2),start*10000000/48000,int64_t(data.size()/2)*10000000/48000);
    }
    void Finish() { if(writer_) { auto writer=std::move(writer_); check_hresult(writer->Finalize()); } }
private:
    void Sample(DWORD stream,const void* data,DWORD size,int64_t time,int64_t duration) {
        com_ptr<IMFMediaBuffer> buffer; check_hresult(MFCreateMemoryBuffer(size,buffer.put()));
        BYTE* bytes=nullptr; check_hresult(buffer->Lock(&bytes,nullptr,nullptr));
        std::memcpy(bytes,data,size); check_hresult(buffer->Unlock()); check_hresult(buffer->SetCurrentLength(size));
        com_ptr<IMFSample> sample; check_hresult(MFCreateSample(sample.put()));
        check_hresult(sample->AddBuffer(buffer.get())); check_hresult(sample->SetSampleTime(time));
        check_hresult(sample->SetSampleDuration(duration)); check_hresult(writer_->WriteSample(stream,sample.get()));
    }
    com_ptr<IMFSinkWriter> writer_;
    DWORD video_{},audio_{};
    int width_{},height_{};
};
void RecordScreen(const RecordingOptions& options,RecordingState& state) {
    if(options.format==RecordingFormat::Gif) { RecordGif(options,state); return; }
    bool apartment=false,mf=false,hasVideo=false;
    std::unique_ptr<MovieWriter> writer;
    const std::wstring temporary=options.path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".partial.mp4";
    std::wstring error,saved;
    const wchar_t* stage=L"初始化";
    try {
        init_apartment(apartment_type::multi_threaded); apartment=true;
        check_hresult(MFStartup(MF_VERSION)); mf=true;
        stage=L"屏幕采集初始化";
        // Cache clean desktop pixels. Choose the pointer for each encoded frame,
        // so switching tools cannot retain a cursor burned into an older frame.
        BorderlessCapture capture(options.region,false);
        std::unique_ptr<AudioSource> system,mic;
        state.systemEnabled=options.systemAudio; state.micEnabled=options.microphone;
        stage=L"音频设备初始化";
        if(options.systemAudio) system=std::make_unique<AudioSource>(false,options.systemDevice);
        if(options.microphone) mic=std::make_unique<AudioSource>(true,options.microphoneDevice);
        stage=L"MP4 编码器初始化";
        // Reserve the unique path before giving it to Media Foundation. Only
        // files actually owned by this session may be deleted on discard.
        HANDLE reserved=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(reserved==INVALID_HANDLE_VALUE) throw_last_error();
        CloseHandle(reserved);
        { std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles.push_back(temporary); }
        writer=std::make_unique<MovieWriter>(temporary,options.region.right-options.region.left,options.region.bottom-options.region.top,true);
        state.ready=true;
        while(options.waitForStart && !state.start && !state.stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        AnnotationRenderer renderer;
        stage=L"采集和编码";
        Timeline timeline; const int64_t started=ClockNow(); timeline.Start(started);
        int64_t cutoff=started,nextFrame=0,audioPosition=0,lastCapture=started,nextCapture=0;
        bool paused=false; Image frame;
        auto flushAudio=[&](int64_t elapsed) {
            const int64_t limit=elapsed*48000/10000000;
            while(audioPosition+480<=limit) {
                std::vector<float> mixed(960,0);
                if(system && state.systemEnabled) system->Mix(audioPosition,480,mixed);
                if(mic && state.micEnabled) mic->Mix(audioPosition,480,mixed);
                writer->Audio(mixed,audioPosition); audioPosition+=480;
            }
        };
        while(!state.stop.load()) {
            const int64_t now=ClockNow(); const bool wantsPause=state.paused.load();
            if(wantsPause!=paused) {
                if(wantsPause) { timeline.Pause(now); flushAudio(timeline.Elapsed(now)); }
                else { timeline.Resume(now); cutoff=now; }
                paused=wantsPause;
            }
            if(state.systemEnabled && !system) system=std::make_unique<AudioSource>(false,options.systemDevice);
            if(state.micEnabled && !mic) mic=std::make_unique<AudioSource>(true,options.microphoneDevice);
            if(system) system->Poll(timeline,paused || !state.systemEnabled,cutoff);
            if(mic) mic->Poll(timeline,paused || !state.micEnabled,cutoff);
            if(now>=nextCapture) {
                Image fresh;
                if(capture.Next(fresh)) { frame=std::move(fresh); lastCapture=now; }
                nextCapture=(now/(10000000/30)+1)*(10000000/30);
            }
            if(now-lastCapture>100000000) throw hresult_error(E_ABORT,L"屏幕采集超过十秒没有返回画面，录制已停止。");
            const int64_t elapsed=timeline.Elapsed(now); state.elapsed=elapsed;
            if(!paused && !frame.Empty()) {
                if(elapsed>=nextFrame) {
                    const int64_t index=elapsed*30/10000000;
                    const int64_t timestamp=index*10000000/30;
                    std::shared_ptr<const RecordingAnnotations> snapshot;
                    bool laserMode=false; LaserPointer laser;
                    { std::lock_guard<std::mutex> lock(state.mutex); snapshot=state.annotations; laserMode=state.laserMode; laser=state.liveLaser; }
                    Image composed=frame;
                    const RecordingAnnotations empty;
                    const bool cursor=state.cursorEnabled;
                    const LaserPointer hidden{};
                    CompositeRecording(composed,snapshot?*snapshot:empty,renderer,now,cursor && laserMode?&laser:&hidden);
                    if(cursor && !laserMode) capture.DrawCursor(composed);
                    writer->Video(composed,timestamp,10000000/30);
                    hasVideo=true;
                    nextFrame=(index+1)*10000000/30;
                }
                flushAudio(std::max<int64_t>(0,elapsed-1000000));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if(system) system->Poll(timeline,paused || !state.systemEnabled,cutoff);
        if(mic) mic->Poll(timeline,paused || !state.micEnabled,cutoff);
        if(hasVideo) flushAudio(timeline.Elapsed(ClockNow()));
    } catch(const hresult_error& e) { error=ErrorMessage(stage,e.code())+L"\n"+std::wstring(e.message()); }
    catch(const std::exception& e) { error=std::wstring(stage)+L"失败："+std::wstring(winrt::to_hstring(e.what())); }
    catch(...) { error=L"录制已停止：未知系统错误。"; }
    bool finalized=false;
    if(writer) {
        try { writer->Finish(); finalized=hasVideo; }
        catch(const hresult_error& e) { error+=L"\n"+ErrorMessage(L"视频封装失败",e.code()); }
        catch(...) { error+=L"\n视频封装失败。"; }
        writer.reset();
    }
    if(state.stopReason==RecordingStopReason::Discard) {
        // The owner deletes files after joining, including failed finalization.
    } else if(finalized) {
        if(MoveFileExW(temporary.c_str(),options.path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            saved=options.path;
            std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles={saved};
        }
        else { saved=temporary; error+=L"\n无法移动到目标路径，已保留可播放的临时视频。"; }
    } else if(!hasVideo && state.stop && error.empty()) {
        if(DeleteFileW(temporary.c_str())) { std::lock_guard<std::mutex> lock(state.mutex); state.ownedFiles.clear(); }
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND) error=ErrorMessage(L"删除未开始的录制失败",HRESULT_FROM_WIN32(GetLastError()))+L"\n保留路径："+temporary;
    } else if(GetFileAttributesW(temporary.c_str())!=INVALID_FILE_ATTRIBUTES) {
        // Retain incomplete output for recovery; never claim it is playable.
        error+=L"\n未完成的临时文件保留于："+temporary;
    }
    if(!hasVideo && !state.stop && error.empty()) error=L"未采集到视频画面。";
    if(mf) MFShutdown(); if(apartment) uninit_apartment();
    { std::lock_guard<std::mutex> lock(state.mutex); state.error=std::move(error); state.savedPath=std::move(saved); }
    state.finished=true;
}
std::wstring DiscardRecordingFiles(RecordingState& state) {
    if(!state.finished) throw std::logic_error("Recording worker must finish before discarding files");
    std::lock_guard<std::mutex> lock(state.mutex);
    std::wstring error; std::vector<std::wstring> remaining;
    for(const auto& path:state.ownedFiles) {
        if(DeleteFileW(path.c_str())) continue;
        const DWORD code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND) continue;
        remaining.push_back(path);
        error+=ErrorMessage(L"删除本次录制失败",HRESULT_FROM_WIN32(code))+L"\n保留路径："+path+L"\n";
    }
    state.ownedFiles=std::move(remaining);
    if(state.ownedFiles.empty()) state.savedPath.clear();
    return error;
}

}



