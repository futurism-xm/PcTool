#include "shared/async/cancellation.h"
#include "ocr/ocr_service.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <cmath>
#include <thread>
#include <fstream>
#ifdef PCTOOL_OCR_EVALUATION
#include "ocr/ocr_evaluation.h"
#include <chrono>
#include <psapi.h>
#endif

namespace capture {
namespace {
#ifdef PCTOOL_OCR_EVALUATION
OcrEvaluationOptions evaluation;
OcrEvaluationTiming timing;
using Clock=std::chrono::steady_clock;
double Elapsed(Clock::time_point start) {return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
#endif
bool UsesV6() {
#ifdef PCTOOL_OCR_EVALUATION
    return evaluation.v6;
#else
    return true;
#endif
}
std::filesystem::path AppDirectory() {
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768)) throw std::runtime_error("Cannot locate OCR models");
    return std::filesystem::path(path).parent_path();
}
std::wstring Wide(const std::string& value) {
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    std::wstring result(size,L'\0');if(size) MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),result.data(),size);return result;
}
struct Engine {
    std::unique_ptr<Ort::Env> environment;
    std::unique_ptr<Ort::Session> detector,recognizer;
    std::vector<std::wstring> characters;
    Engine() {
        const auto directory=AppDirectory();
        // Load only the packaged DLL. A missing runtime must not prevent PcTool startup.
        static HMODULE module=LoadLibraryExW((directory/L"onnxruntime.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) throw std::runtime_error("Missing or incompatible onnxruntime.dll next to PcTool.exe");
        auto getApi=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(module,"OrtGetApiBase"));
        const OrtApi* api=getApi?getApi()->GetApi(ORT_API_VERSION):nullptr;
        if(!api) throw std::runtime_error("Unsupported ONNX Runtime API version");
        // The runtime remains loaded while any ORT object may exist.
        Ort::InitApi(api);environment=std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,"PcTool OCR");
        Ort::SessionOptions options;options.SetIntraOpNumThreads(2);options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto modelDirectory=directory/L"ocr_models";
#ifdef PCTOOL_OCR_EVALUATION
        if(!evaluation.modelDirectory.empty())modelDirectory=evaluation.modelDirectory;
#endif
        detector=std::make_unique<Ort::Session>(*environment,(modelDirectory/L"det.onnx").c_str(),options);
        recognizer=std::make_unique<Ort::Session>(*environment,(modelDirectory/L"rec.onnx").c_str(),options);
        Ort::AllocatorWithDefaultOptions allocator;
        auto dictionary=recognizer->GetModelMetadata().LookupCustomMetadataMapAllocated("character",allocator);
        std::string dictionaryText=dictionary?dictionary.get():"";
        if(UsesV6()) {std::ifstream file(modelDirectory/L"characters.txt",std::ios::binary);if(!file)throw std::runtime_error("Missing OCR character dictionary: ocr_models/characters.txt");dictionaryText.assign(std::istreambuf_iterator<char>(file),{});}
        if(dictionaryText.empty()) throw std::runtime_error("OCR recognition model lacks its character dictionary");
        characters.emplace_back();std::istringstream lines(dictionaryText);std::string line;
        while(std::getline(lines,line)) {if(!line.empty() && line.back()=='\r')line.pop_back();characters.push_back(Wide(line));}
        characters.push_back(L" ");
    }
};
std::mutex engineMutex;

// BGR input: v6 detection uses ImageNet mean/std; recognition uses .5/.5.
std::vector<float> Input(const Image& image,RECT source,int width,int height,int paddedWidth,const Cancellation& cancel,bool detection=false) {
    std::vector<float> result(size_t(paddedWidth)*height*3,0.0F);
    const int sw=source.right-source.left,sh=source.bottom-source.top;
    for(int y=0;y<height;++y) {
        if(cancel.requested) return {};
        double yy=std::clamp((y+0.5)*sh/height-0.5,0.0,double(sh-1));int y0=int(yy),y1=std::min(sh-1,y0+1);float fy=float(yy-y0);
        for(int x=0;x<width;++x) {
            double xx=std::clamp((x+0.5)*sw/width-0.5,0.0,double(sw-1));int x0=int(xx),x1=std::min(sw-1,x0+1);float fx=float(xx-x0);
            const uint32_t p[]={image.pixels[size_t(source.top+y0)*image.width+source.left+x0],image.pixels[size_t(source.top+y0)*image.width+source.left+x1],image.pixels[size_t(source.top+y1)*image.width+source.left+x0],image.pixels[size_t(source.top+y1)*image.width+source.left+x1]};
            for(int c=0;c<3;++c) {
                float a=float((p[0]>>(c*8))&255)*(1-fx)+float((p[1]>>(c*8))&255)*fx;
                float b=float((p[2]>>(c*8))&255)*(1-fx)+float((p[3]>>(c*8))&255)*fx;
                result[(size_t(c)*height+y)*paddedWidth+x]=((a*(1-fy)+b*fy)/255.0F-0.5F)*2;
                if(detection && UsesV6()) {const float mean[]={.485F,.456F,.406F},stddev[]={.229F,.224F,.225F};result[(size_t(c)*height+y)*paddedWidth+x]=((a*(1-fy)+b*fy)/255.0F-mean[c])/stddev[c];}
            }
        }
    }
    return result;
}
Ort::Value Run(Ort::Session& session,std::vector<float>& input,int width,int height,const Cancellation& cancel) {
    if(cancel.requested) throw std::runtime_error("OCR cancelled");
    auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    const int64_t shape[]={1,3,height,width};auto value=Ort::Value::CreateTensor<float>(memory,input.data(),input.size(),shape,4);
    Ort::AllocatorWithDefaultOptions allocator;auto in=session.GetInputNameAllocated(0,allocator),out=session.GetOutputNameAllocated(0,allocator);
    const char* inputs[]={in.get()};const char* outputs[]={out.get()};Ort::RunOptions options;
    // Terminate an in-flight CPU run when the progress window is cancelled.
    std::atomic<bool> finished{false};std::thread watcher([&] {while(!finished) {if(cancel.requested){try{options.SetTerminate();}catch(...){}break;}Sleep(10);}});
    struct Join {std::atomic<bool>& flag;std::thread& worker;~Join(){flag=true;worker.join();}} join{finished,watcher};
    auto values=session.Run(options,inputs,&value,1,outputs,1);return std::move(values.front());
}
std::vector<RECT> Detect(Engine& engine,const Image& image,int top,int bottom,const Cancellation& cancel) {
    const int sourceHeight=bottom-top;
    const double scale=std::min(1.5,2048.0/std::max(image.width,sourceHeight));
    const int width=std::max(32,int(std::round(image.width*scale/32))*32),height=std::max(32,int(std::round(sourceHeight*scale/32))*32);
    float threshold=.2F,boxThreshold=.4F,unclip=1.4F;
#ifdef PCTOOL_OCR_EVALUATION
    if(evaluation.v6) {threshold=evaluation.threshold;boxThreshold=evaluation.boxThreshold;unclip=evaluation.unclip;}
    else {threshold=.3F;boxThreshold=.5F;unclip=1.5F;}
#endif
    auto input=Input(image,{0,top,image.width,bottom},width,height,width,cancel,true);if(input.empty()) return {};
    auto output=Run(*engine.detector,input,width,height,cancel);auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
    if(shape.size()!=4 || shape[0]!=1 || shape[1]!=1) throw std::runtime_error("Unexpected OCR detection model output");
    int h=int(shape[2]),w=int(shape[3]);const float* scores=output.GetTensorData<float>();
    std::vector<uint8_t> visited(size_t(w)*h);std::vector<int> queue;std::vector<RECT> boxes;
    for(int y=0;y<h;++y) {
        if(cancel.requested) return {};
        for(int x=0;x<w;++x) {
            int first=y*w+x;if(visited[first] || scores[first]<threshold)continue;
            queue.clear();queue.push_back(first);visited[first]=1;int left=x,right=x,up=y,down=y;double score=0;
            for(size_t q=0;q<queue.size();++q) {
                int index=queue[q],xx=index%w,yy=index/w;score+=scores[index];left=std::min(left,xx);right=std::max(right,xx);up=std::min(up,yy);down=std::max(down,yy);
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
                    int nx=xx+dx,ny=yy+dy;if(nx<0||ny<0||nx>=w||ny>=h)continue;int next=ny*w+nx;
                    if(!visited[next] && scores[next]>=threshold){visited[next]=1;queue.push_back(next);}
                }
            }
            if(queue.size()<6 || down-up<2 || score/queue.size()<boxThreshold) continue;
            // DB shrinks line regions during training; expand the horizontal line box.
            double bw=right-left+1,bh=down-up+1,pad=bw*bh*unclip/(2*(bw+bh));
            RECT r{LONG(std::floor((left-pad)*image.width/w)),LONG(top+std::floor((up-pad)*sourceHeight/h)),LONG(std::ceil((right+1+pad)*image.width/w)),LONG(top+std::ceil((down+1+pad)*sourceHeight/h))};
            r.left=std::clamp<LONG>(r.left,0,image.width);r.right=std::clamp<LONG>(r.right,0,image.width);
            r.top=std::clamp<LONG>(r.top,top,bottom);r.bottom=std::clamp<LONG>(r.bottom,top,bottom);
            if(r.right-r.left>=3 && r.bottom-r.top>=3) boxes.push_back(r);
        }
    }
    std::sort(boxes.begin(),boxes.end(),[](RECT a,RECT b){return a.top!=b.top?a.top<b.top:a.left<b.left;});return boxes;
}
std::wstring RecognizeLine(Engine& engine,const Image& image,RECT rect,const Cancellation& cancel) {
    const int width=std::clamp(int(std::ceil(48.0*(rect.right-rect.left)/(rect.bottom-rect.top))),8,8192),padded=std::max(320,(width+7)/8*8);
    auto input=Input(image,rect,width,48,padded,cancel);if(input.empty())return {};
    auto output=Run(*engine.recognizer,input,padded,48,cancel);auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
    if(shape.size()!=3 || shape[0]!=1 || shape[2]!=int64_t(engine.characters.size())) throw std::runtime_error("OCR dictionary/model dimensions do not match");
    const float* scores=output.GetTensorData<float>();std::wstring text;int previous=-1;double confidence=0;size_t count=0;
    for(int64_t t=0;t<shape[1];++t) {
        const float* row=scores+t*shape[2];int best=int(std::max_element(row,row+shape[2])-row);
        if(best && best!=previous) {text+=engine.characters[best];confidence+=row[best];++count;}previous=best;
    }
    return count && confidence/count>=0.5?text:L"";
}
}
std::wstring LocalOcrModelName() {
#ifdef PCTOOL_OCR_EVALUATION
    return evaluation.modelName;
#else
    return L"PP-OCRv6 tiny";
#endif
}
#ifdef PCTOOL_OCR_EVALUATION
void ConfigureOcrEvaluation(const OcrEvaluationOptions& options) {evaluation=options;}
OcrEvaluationTiming LastOcrEvaluationTiming() {return timing;}
#endif
OcrDocument RecognizeLocalDocument(const Image& image,const Cancellation& cancel) {
    if(image.Empty() || cancel.requested)return {};
    std::unique_lock<std::mutex> lock(engineMutex,std::defer_lock);
    while(!lock.try_lock()){if(cancel.requested)return {};Sleep(15);}
#ifdef PCTOOL_OCR_EVALUATION
    timing={};auto loadStart=Clock::now();
#endif
    static Engine engine;
#ifdef PCTOOL_OCR_EVALUATION
    timing.loadMs=Elapsed(loadStart);
    PROCESS_MEMORY_COUNTERS_EX loaded{};loaded.cb=sizeof(loaded);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&loaded),sizeof(loaded));timing.loadedWorking=loaded.WorkingSetSize;timing.loadedPrivate=loaded.PrivateUsage;
#endif
    std::vector<OcrTextLine> lines;constexpr int tileHeight=1024,overlap=128,step=tileHeight-overlap;
    for(int top=0;top<image.height;top+=step) {
        if(cancel.requested)return {};
        int bottom=std::min(image.height,top+tileHeight);bool last=bottom==image.height;
#ifdef PCTOOL_OCR_EVALUATION
        auto detectStart=Clock::now();
#endif
        auto boxes=Detect(engine,image,top,bottom,cancel);
#ifdef PCTOOL_OCR_EVALUATION
        timing.detectMs+=Elapsed(detectStart);
#endif
        for(auto box:boxes) {
            if(cancel.requested)return {};
            const double center=(box.top+box.bottom)/2.0;
            if(center<(top?top+overlap/2:0) || center>=(last?image.height:top+step+overlap/2))continue;
#ifdef PCTOOL_OCR_EVALUATION
            auto recStart=Clock::now();
#endif
            auto text=RecognizeLine(engine,image,box,cancel);
#ifdef PCTOOL_OCR_EVALUATION
            timing.recognizeMs+=Elapsed(recStart);
#endif
            if(text.empty())continue;
            AppendOcrTextLines(lines,{{double(box.left),double(box.top),double(box.right-box.left),double(box.bottom-box.top),std::move(text)}},0,0,image.height);
        }
        if(last)break;
    }
    return cancel.requested?OcrDocument{}:GroupOcrLines(image.width,image.height,std::move(lines));
}
}
