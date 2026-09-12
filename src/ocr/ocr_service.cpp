#include "shared/async/cancellation.h"
#include "ocr/ocr_service.h"
#include "shared/ui/capture_ui.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <future>
#include <chrono>

namespace capture {
using namespace winrt;
using namespace Windows::Media::Ocr;
std::vector<OcrLanguage> OcrLanguages() {
    std::vector<OcrLanguage> result;
    for(const auto& l:OcrEngine::AvailableRecognizerLanguages()) result.push_back({l.LanguageTag().c_str(),l.DisplayName().c_str()});
    return result;
}
OcrDocument RecognizeDocument(const Image& image,const std::wstring& language,const Cancellation& cancel) {
    if(image.Empty()) return {};
    auto engine=OcrEngine::TryCreateFromLanguage(Windows::Globalization::Language(language));
    if(!engine) throw hresult_error(E_NOTIMPL,L"未安装所选 OCR 语言。请在 Windows 设置 → 时间和语言中安装相应语言的文字识别组件。");
    const int maxSide=int(OcrEngine::MaxImageDimension());
    // Horizontal text lines need their full width. Only exceptionally wide images are scaled.
    const double ratio=std::min(1.0,double(maxSide)/image.width);
    const int width=std::max(1,int(image.width*ratio));
    const int tileHeight=std::max(1,int(maxSide/ratio));
    const int overlap=std::min(tileHeight/4,128);
    const int step=tileHeight-overlap;
    std::vector<OcrTextLine> lines;
    for(int y=0;y<image.height;y+=step) {
        if(cancel.requested.load()) return {};
        const int sourceHeight=std::min(tileHeight,image.height-y);
        const int height=std::max(1,int(sourceHeight*ratio));
        std::vector<uint8_t> bytes(size_t(width)*height*4);
        for(int row=0;row<height;++row) for(int x=0;x<width;++x) {
            const uint32_t p=image.pixels[size_t(y+std::min(sourceHeight-1,int(row/ratio)))*image.width+std::min(image.width-1,int(x/ratio))]|0xff000000;
            std::memcpy(bytes.data()+(size_t(row)*width+x)*4,&p,4);
        }
        Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(bytes);
        auto bitmap=Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(),
            Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,width,height,Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
        auto operation=engine.RecognizeAsync(bitmap);
        while(operation.Status()==Windows::Foundation::AsyncStatus::Started) {
            if(cancel.requested.load()) { operation.Cancel(); return {}; }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        auto result=operation.GetResults();
        std::vector<OcrTextLine> tileLines;
        for(const auto& line:result.Lines()) {
            double top=height,bottom=0,left=width,right=0;
            for(const auto& word:line.Words()) { auto r=word.BoundingRect(); left=std::min(left,double(r.X)); right=std::max(right,double(r.X+r.Width)); top=std::min(top,double(r.Y)); bottom=std::max(bottom,double(r.Y+r.Height)); }
            tileLines.push_back({left/ratio,top/ratio,(right-left)/ratio,(bottom-top)/ratio,line.Text().c_str()});
        }
        const bool last=y+sourceHeight>=image.height;
        AppendOcrTextLines(lines,tileLines,y,y?y+overlap/2:0,last?image.height:y+step+overlap/2);
        if(last) break;
    }
    return GroupOcrLines(image.width,image.height,std::move(lines));
}
std::wstring RecognizeImage(const Image& image,const std::wstring& language,const Cancellation& cancellation) {
    return RecognizeDocument(image,language,cancellation).text;
}
}
