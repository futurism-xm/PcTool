#include "shared/async/cancellation.h"
#include "ocr/ocr_evaluation.h"
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <future>
using namespace capture;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
std::string Utf8(const std::wstring& s) {int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;}
PROCESS_MEMORY_COUNTERS_EX Memory(){PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m));return m;}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=6)throw std::runtime_error("Usage: PcToolOcrBenchmark model-dir v5|tiny|small|medium corpus-dir output-dir repeats");
        std::wstring tier=argv[2];if(tier!=L"v5"&&tier!=L"tiny"&&tier!=L"small"&&tier!=L"medium")throw std::runtime_error("Unknown model tier");
        OcrEvaluationOptions options;options.modelDirectory=argv[1];options.v6=tier!=L"v5";options.modelName=options.v6?L"PP-OCRv6 "+tier:L"PP-OCRv5 mobile";options.boxThreshold=tier==L"tiny"?.4F:.45F;ConfigureOcrEvaluation(options);
        fs::path corpus=argv[3],output=argv[4];fs::create_directories(output);int repeats=_wtoi(argv[5]);if(repeats<1||repeats>10)throw std::runtime_error("Invalid repeats");
        auto baseline=Memory();std::ofstream csv(output/L"metrics.csv");csv<<"sample,repeat,total_ms,load_ms,detect_ms,recognize_ms,working_bytes,private_bytes,peak_working_bytes,peak_private_bytes,baseline_working_bytes,baseline_private_bytes,lines,loaded_working_bytes,loaded_private_bytes\n";
        std::vector<fs::path> samples;for(auto& entry:fs::directory_iterator(corpus))if(entry.path().extension()==L".bgra")samples.push_back(entry.path());std::sort(samples.begin(),samples.end());if(samples.empty())throw std::runtime_error("Empty corpus");
        Image last;
        for(auto& path:samples){std::ifstream file(path,std::ios::binary);int w=0,h=0;file.read(reinterpret_cast<char*>(&w),4);file.read(reinterpret_cast<char*>(&h),4);if(w<=0||h<=0||uint64_t(w)*h>kImageBudget/4)throw std::runtime_error("Invalid fixture dimensions");Image image(w,h);file.read(reinterpret_cast<char*>(image.pixels.data()),image.pixels.size()*4);if(!file)throw std::runtime_error("Truncated fixture");
            for(int repeat=0;repeat<repeats;++repeat){std::atomic<bool> done{false};auto before=Memory();SIZE_T peakWorking=before.WorkingSetSize,peakPrivate=before.PrivateUsage;
                std::thread sampler([&]{while(!done){auto m=Memory();peakWorking=std::max(peakWorking,m.WorkingSetSize);peakPrivate=std::max(peakPrivate,m.PrivateUsage);Sleep(5);}});
                Cancellation cancel;auto start=Clock::now();OcrDocument result;try{result=RecognizeLocalDocument(image,cancel);}catch(...){done=true;sampler.join();throw;}double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();done=true;sampler.join();auto after=Memory();auto timing=LastOcrEvaluationTiming();size_t lines=0;
                std::ofstream text(output/(path.stem().wstring()+L"-"+std::to_wstring(repeat)+L".txt"),std::ios::binary);
                std::ofstream boxes(output/(path.stem().wstring()+L"-"+std::to_wstring(repeat)+L".boxes"));
                for(auto& paragraph:result.paragraphs)for(auto& line:paragraph.lines){++lines;if(line.x<0||line.y<0||line.x+line.width>w||line.y+line.height>h)throw std::runtime_error("Invalid result coordinates");boxes<<line.x<<'\t'<<line.y<<'\t'<<line.width<<'\t'<<line.height<<'\t'<<Utf8(line.text)<<'\n';}
                text<<Utf8(result.text);csv<<path.stem().string()<<','<<repeat<<','<<elapsed<<','<<timing.loadMs<<','<<timing.detectMs<<','<<timing.recognizeMs<<','<<after.WorkingSetSize<<','<<after.PrivateUsage<<','<<peakWorking<<','<<peakPrivate<<','<<baseline.WorkingSetSize<<','<<baseline.PrivateUsage<<','<<lines<<','<<timing.loadedWorking<<','<<timing.loadedPrivate<<'\n';csv.flush();
            }
            std::cout<<path.stem().string()<<" done"<<std::endl;last=std::move(image);
        }
        Cancellation stop;auto worker=std::async(std::launch::async,[&]{try{RecognizeLocalDocument(last,stop);}catch(...){}});Sleep(30);auto cancelled=Clock::now();stop.requested=true;if(worker.wait_for(std::chrono::seconds(2))!=std::future_status::ready)throw std::runtime_error("Cancellation exceeded 2s");worker.get();
        std::ofstream(output/L"checks.txt")<<"coordinate bounds PASS\ncancellation PASS "<<std::chrono::duration<double,std::milli>(Clock::now()-cancelled).count()<<" ms\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
