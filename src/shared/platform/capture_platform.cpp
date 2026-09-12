#include "shared/platform/app_storage.h"
#include "shared/platform/capture_platform.h"
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <cstring>
#include <stdexcept>

namespace capture {
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
FrozenDesktop FreezeDesktop() {
    RECT bounds{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
    const int width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    Image image(width,height); bounds.right=bounds.left+width; bounds.bottom=bounds.top+height;
    HDC screen=GetDC(nullptr),memory=screen?CreateCompatibleDC(screen):nullptr;
    HBITMAP bitmap=ToBitmap(image); auto old=memory&&bitmap?SelectObject(memory,bitmap):nullptr;
    const bool ok=old && BitBlt(memory,0,0,width,height,screen,bounds.left,bounds.top,SRCCOPY|CAPTUREBLT);
    if(old) SelectObject(memory,old); if(memory) DeleteDC(memory); if(screen) ReleaseDC(nullptr,screen);
    if(!ok) {if(bitmap)DeleteObject(bitmap);throw std::runtime_error("Cannot capture desktop");}
    try {image=FromBitmap(bitmap);} catch(...) {DeleteObject(bitmap);throw;}
    DeleteObject(bitmap);return {std::move(image),bounds};
}
Image FromBitmap(HBITMAP bitmap) {
    BITMAP b{};
    if(!bitmap || !GetObjectW(bitmap,sizeof(b),&b)) throw std::runtime_error("Cannot read bitmap");
    Image image(b.bmWidth,b.bmHeight);
    BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),b.bmWidth,-b.bmHeight,1,32,BI_RGB};
    HDC dc=GetDC(nullptr);
    const int rows=GetDIBits(dc,bitmap,0,b.bmHeight,image.pixels.data(),&info,DIB_RGB_COLORS);
    ReleaseDC(nullptr,dc);
    if(rows!=b.bmHeight) throw std::runtime_error("Cannot read bitmap pixels");
    for(auto& p:image.pixels) p|=0xff000000;
    return image;
}
HBITMAP ToBitmap(const Image& image) {
    if(image.Empty()) return nullptr;
    BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),image.width,-image.height,1,32,BI_RGB};
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(bitmap) std::memcpy(pixels,image.pixels.data(),image.pixels.size()*4);
    return bitmap;
}
bool CopyImage(HWND owner,const Image& image) {
    HBITMAP bitmap=ToBitmap(image);
    if(!bitmap) return false;
    if(!OpenClipboard(owner)) { DeleteObject(bitmap); return false; }
    EmptyClipboard(); const bool ok=SetClipboardData(CF_BITMAP,bitmap)!=nullptr;
    CloseClipboard(); if(!ok) DeleteObject(bitmap); return ok;
}
bool CopyImageFile(HWND owner,const Image& image) {
    HBITMAP bitmap=ToBitmap(image);if(!bitmap)return false;
    std::filesystem::path path;HGLOBAL files{};
    try { path=app_storage::Unique(L"Screenshots",L".png");
        if(!SavePng(image,path.wstring()))throw std::runtime_error("PNG encoding failed");
        files=app_storage::FileDrop(path);if(!files)throw std::bad_alloc();
    }catch(...){DeleteObject(bitmap);if(!path.empty())DeleteFileW(path.c_str());return false;}
    if(!OpenClipboard(owner)){GlobalFree(files);DeleteObject(bitmap);DeleteFileW(path.c_str());return false;}
    bool ok=EmptyClipboard()!=FALSE;
    if(ok){ok=SetClipboardData(CF_HDROP,files)!=nullptr;if(ok)files=nullptr;}
    if(ok){ok=SetClipboardData(CF_BITMAP,bitmap)!=nullptr;if(ok)bitmap=nullptr;}
    if(!ok)EmptyClipboard();CloseClipboard();if(files)GlobalFree(files);if(bitmap)DeleteObject(bitmap);
    if(!ok)DeleteFileW(path.c_str());return ok;
}
bool CopyText(HWND owner,const std::wstring& text) {
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(!memory) return false;
    void* target=GlobalLock(memory);
    if(!target) { GlobalFree(memory); return false; }
    std::memcpy(target,text.c_str(),(text.size()+1)*sizeof(wchar_t)); GlobalUnlock(memory);
    if(!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    EmptyClipboard(); const bool ok=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
    CloseClipboard(); if(!ok) GlobalFree(memory); return ok;
}
bool SavePng(const Image& image,const std::wstring& path) {
    try {
        auto factory=create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        com_ptr<IWICStream> stream; check_hresult(factory->CreateStream(stream.put()));
        check_hresult(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
        com_ptr<IWICBitmapEncoder> encoder;
        check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));
        check_hresult(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
        com_ptr<IWICBitmapFrameEncode> frame; check_hresult(encoder->CreateNewFrame(frame.put(),nullptr));
        check_hresult(frame->Initialize(nullptr)); check_hresult(frame->SetSize(image.width,image.height));
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        check_hresult(frame->SetPixelFormat(&format));
        check_hresult(frame->WritePixels(image.height,image.width*4,UINT(image.pixels.size()*4),
            reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.pixels.data()))));
        check_hresult(frame->Commit()); check_hresult(encoder->Commit()); return true;
    } catch(...) { return false; }
}
std::wstring ChoosePath(HWND owner,bool video) {
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t path[32768]{};
    swprintf_s(path,L"PcTool_%04u%02u%02u_%02u%02u%02u.%s",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,video?L"mp4":L"png");
    OPENFILENAMEW d{}; d.lStructSize=sizeof(d); d.hwndOwner=owner;
    d.lpstrFilter=video?L"MP4 视频\0*.mp4\0":L"PNG 图片\0*.png\0";
    d.lpstrFile=path; d.nMaxFile=32768; d.lpstrDefExt=video?L"mp4":L"png";
    d.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&d)?path:L"";
}
std::wstring ErrorMessage(const wchar_t* action,long code) {
    wchar_t hex[32]{}; swprintf_s(hex,L" (0x%08lX)",code);
    return std::wstring(action)+hex+L"\n"+hresult_error(code).message().c_str();
}
std::wstring CurrentError(const wchar_t* action) {
    try { throw; }
    catch(const hresult_error& e) { return ErrorMessage(action,e.code()); }
    catch(const std::bad_alloc&) { return std::wstring(action)+L"：内存或图形资源不足。"; }
    catch(const std::exception& e) { return std::wstring(action)+L"："+to_hstring(e.what()).c_str(); }
    catch(...) { return std::wstring(action)+L"：未知系统错误。"; }
}
int64_t ClockNow() {
    LARGE_INTEGER t{},f{}; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return t.QuadPart/f.QuadPart*10000000+(t.QuadPart%f.QuadPart)*10000000/f.QuadPart;
}
bool CheckCaptureRegion(RECT r,std::wstring& error) {
    using VersionFunction=LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto versionFunction=reinterpret_cast<VersionFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    OSVERSIONINFOW version{}; version.dwOSVersionInfoSize=sizeof(version);
    if(!versionFunction || versionFunction(&version)!=0 || version.dwMajorVersion<10 || version.dwBuildNumber<19041) {
        error=L"长截图和录屏需要 Windows 10 2004（19041）或更新版本。"; return false;
    }
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m=MonitorFromRect(&r,MONITOR_DEFAULTTONULL);
    if(r.right<=r.left || r.bottom<=r.top || !m || !GetMonitorInfoW(m,&mi) ||
       r.left<mi.rcMonitor.left || r.top<mi.rcMonitor.top || r.right>mi.rcMonitor.right || r.bottom>mi.rcMonitor.bottom) {
        error=L"请在同一个显示器内选择录制或长截图区域。"; return false;
    }
    return true;
}
struct DesktopCapture::Impl {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Texture2D> staging;
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    RECT crop{}, monitorRect{};
    HMONITOR monitor{};
    Impl(RECT r,bool cursor) {
        if(!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_NOTIMPL,L"系统不支持屏幕采集，需要 Windows 10 2004 或更新版本。");
        std::wstring error;
        if(!CheckCaptureRegion(r,error)) throw hresult_error(E_INVALIDARG,error);
        check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
        auto dxgi=device.as<IDXGIDevice>(); com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        monitor=MonitorFromRect(&r,MONITOR_DEFAULTTONULL);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(monitor,&mi); monitorRect=mi.rcMonitor;
        crop={r.left-mi.rcMonitor.left,r.top-mi.rcMonitor.top,r.right-mi.rcMonitor.left,r.bottom-mi.rcMonitor.top};
        auto factory=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        check_hresult(factory->CreateForMonitor(monitor,guid_of<GraphicsCaptureItem>(),put_abi(item)));
        pool=Direct3D11CaptureFramePool::CreateFreeThreaded(inspectable.as<IDirect3DDevice>(),DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());
        session=pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(cursor);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=crop.right-crop.left; desc.Height=crop.bottom-crop.top;
        desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check_hresult(device->CreateTexture2D(&desc,nullptr,staging.put())); session.StartCapture();
    }
    ~Impl() {
        // Device/display removal can race shutdown; cleanup must never terminate the app.
        try { if(session) session.Close(); } catch(...) {}
        try { if(pool) pool.Close(); } catch(...) {}
    }
};
DesktopCapture::DesktopCapture(RECT r,bool cursor) : impl_(std::make_unique<Impl>(r,cursor)) {}
DesktopCapture::~DesktopCapture()=default;
bool DesktopCapture::Next(Image& image) {
    auto& p=*impl_;
    MONITORINFO mi{sizeof(mi)};
    if(!GetMonitorInfoW(p.monitor,&mi) || !EqualRect(&mi.rcMonitor,&p.monitorRect))
        throw hresult_error(E_ABORT,L"显示器布局已改变，请重新选择采集区域。");
    auto frame=p.pool.TryGetNextFrame(); if(!frame) return false;
    auto size=frame.ContentSize();
    if(size.Width!=p.monitorRect.right-p.monitorRect.left || size.Height!=p.monitorRect.bottom-p.monitorRect.top)
        throw hresult_error(E_ABORT,L"显示器分辨率已改变，采集已停止。");
    auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture; check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(),texture.put_void()));
    D3D11_BOX box{UINT(p.crop.left),UINT(p.crop.top),0,UINT(p.crop.right),UINT(p.crop.bottom),1};
    p.context->CopySubresourceRegion(p.staging.get(),0,0,0,0,texture.get(),0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(p.context->Map(p.staging.get(),0,D3D11_MAP_READ,0,&mapped));
    const int w=p.crop.right-p.crop.left,h=p.crop.bottom-p.crop.top;
    // Allocate before mapping in future calls; always unmap if allocation fails.
    try {
        if(image.width!=w || image.height!=h) image=Image(w,h);
        for(int y=0;y<h;++y) std::memcpy(image.pixels.data()+size_t(y)*w,static_cast<BYTE*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(w)*4);
    } catch(...) { p.context->Unmap(p.staging.get(),0); throw; }
    p.context->Unmap(p.staging.get(),0); return true;
}
}

namespace capture {
struct BorderlessCapture::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    winrt::com_ptr<ID3D11Texture2D> staging;
    DXGI_OUTPUT_DESC output{};
    DXGI_OUTDUPL_DESC description{};
    RECT region{},crop{};
    Image desktop;
    bool cursor{},pointerVisible{};
    POINT pointer{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    std::vector<BYTE> pointerPixels;
    Impl(RECT r,bool showCursor):region(r),cursor(showCursor) {
        std::wstring error; if(!CheckCaptureRegion(r,error)) throw winrt::hresult_error(E_INVALIDARG,error);
        const auto monitor=MonitorFromRect(&r,MONITOR_DEFAULTTONULL);
        winrt::com_ptr<IDXGIFactory1> factory; winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void()));
        for(UINT a=0;;++a) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            auto hr=factory->EnumAdapters1(a,adapter.put()); if(hr==DXGI_ERROR_NOT_FOUND) break; winrt::check_hresult(hr);
            for(UINT o=0;;++o) {
                winrt::com_ptr<IDXGIOutput> candidate;
                hr=adapter->EnumOutputs(o,candidate.put()); if(hr==DXGI_ERROR_NOT_FOUND) break; winrt::check_hresult(hr);
                DXGI_OUTPUT_DESC info{}; winrt::check_hresult(candidate->GetDesc(&info)); if(info.Monitor!=monitor) continue;
                output=info;
                winrt::check_hresult(D3D11CreateDevice(adapter.get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
                hr=candidate.as<IDXGIOutput1>()->DuplicateOutput(device.get(),duplication.put());
                if(hr==E_ACCESSDENIED) throw winrt::hresult_error(hr,L"无法访问录制桌面。请解锁 Windows，并关闭安全桌面上的提示后重试。");
                if(FAILED(hr)) throw winrt::hresult_error(hr,L"无法启动无边框录屏。请确认显示器连接正常，且当前处于可采集的交互桌面。");
                duplication->GetDesc(&description);
                crop={r.left-info.DesktopCoordinates.left,r.top-info.DesktopCoordinates.top,r.right-info.DesktopCoordinates.left,r.bottom-info.DesktopCoordinates.top};
                return;
            }
        }
        throw winrt::hresult_error(DXGI_ERROR_NOT_FOUND,L"未找到选区显示器的桌面采集输出。");
    }
    void DrawPointer(Image& image) {
        if(!pointerVisible || pointerPixels.empty()) return;
        const int height=int(shape.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME?shape.Height/2:shape.Height);
        // Duplication reports the shape's upper-left position (not its hotspot).
        for(int y=0;y<height;++y) for(int x=0;x<int(shape.Width);++x) {
            int dx=pointer.x-crop.left+x,dy=pointer.y-crop.top+y;
            if(dx<0 || dy<0 || dx>=image.width || dy>=image.height) continue;
            auto& pixel=image.pixels[size_t(dy)*image.width+dx];
            if(shape.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                const BYTE bit=BYTE(0x80>>(x%8));
                const bool andBit=(pointerPixels[size_t(y)*shape.Pitch+x/8]&bit)!=0;
                const bool xorBit=(pointerPixels[size_t(y+height)*shape.Pitch+x/8]&bit)!=0;
                pixel=0xff000000|((pixel&(andBit?0xffffff:0))^(xorBit?0xffffff:0));
            } else {
                uint32_t source{}; std::memcpy(&source,pointerPixels.data()+size_t(y)*shape.Pitch+x*4,4);
                if(shape.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) pixel=0xff000000|((source>>24)?(pixel^source):(source&0xffffff));
                else {
                    const unsigned alpha=source>>24; uint32_t result=0xff000000;
                    for(int shift:{0,8,16}) result|=((((source>>shift)&255)*alpha+((pixel>>shift)&255)*(255-alpha)+127)/255)<<shift;
                    pixel=result;
                }
            }
        }
    }
};
BorderlessCapture::BorderlessCapture(RECT region,bool cursor):impl_(std::make_unique<Impl>(region,cursor)) {}
BorderlessCapture::~BorderlessCapture()=default;
void BorderlessCapture::DrawCursor(Image& image) { impl_->DrawPointer(image); }
bool BorderlessCapture::Next(Image& image) {
    auto& p=*impl_;
    MONITORINFO mi{sizeof(mi)};
    if(!GetMonitorInfoW(p.output.Monitor,&mi) || !EqualRect(&mi.rcMonitor,&p.output.DesktopCoordinates))
        throw winrt::hresult_error(E_ABORT,L"显示器布局已改变，录屏已停止。请重新选择录制区域。");
    DXGI_OUTDUPL_FRAME_INFO info{}; winrt::com_ptr<IDXGIResource> resource;
    const auto hr=p.duplication->AcquireNextFrame(0,&info,resource.put());
    if(hr==DXGI_ERROR_WAIT_TIMEOUT) {
        if(p.desktop.Empty()) return false;
        image=p.desktop; if(p.cursor) p.DrawPointer(image); return true; // A static desktop is a valid frame.
    }
    if(FAILED(hr)) throw winrt::hresult_error(hr,L"桌面采集已中断（显示器或桌面发生变化），正在尝试保留视频。");
    struct Release { IDXGIOutputDuplication* duplication; ~Release(){duplication->ReleaseFrame();} } release{p.duplication.get()};
    if(info.LastMouseUpdateTime.QuadPart) { p.pointer=info.PointerPosition.Position; p.pointerVisible=info.PointerPosition.Visible!=FALSE; }
    if(info.PointerShapeBufferSize) {
        p.pointerPixels.resize(info.PointerShapeBufferSize); UINT required{};
        winrt::check_hresult(p.duplication->GetFramePointerShape(UINT(p.pointerPixels.size()),p.pointerPixels.data(),&required,&p.shape));
    }
    if(info.LastPresentTime.QuadPart || p.desktop.Empty()) {
        auto texture=resource.as<ID3D11Texture2D>(); D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        const bool rotated=p.description.Rotation!=DXGI_MODE_ROTATION_IDENTITY && p.description.Rotation!=DXGI_MODE_ROTATION_UNSPECIFIED;
        if(!p.staging) {
            if(!rotated) { desc.Width=p.crop.right-p.crop.left; desc.Height=p.crop.bottom-p.crop.top; }
            desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(p.device->CreateTexture2D(&desc,nullptr,p.staging.put()));
        }
        p.desktop=Image(p.crop.right-p.crop.left,p.crop.bottom-p.crop.top);
        if(rotated) p.context->CopyResource(p.staging.get(),texture.get());
        else {
            D3D11_BOX box{UINT(p.crop.left),UINT(p.crop.top),0,UINT(p.crop.right),UINT(p.crop.bottom),1};
            p.context->CopySubresourceRegion(p.staging.get(),0,0,0,0,texture.get(),0,&box);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(p.context->Map(p.staging.get(),0,D3D11_MAP_READ,0,&mapped));
        for(int y=0;y<p.desktop.height;++y) for(int x=0;x<p.desktop.width;++x) {
            const int dx=x+p.crop.left,dy=y+p.crop.top; int sx=rotated?dx:x,sy=rotated?dy:y;
            switch(p.description.Rotation) {
            case DXGI_MODE_ROTATION_ROTATE90: sx=dy; sy=int(desc.Height)-1-dx; break;
            case DXGI_MODE_ROTATION_ROTATE180: sx=int(desc.Width)-1-dx; sy=int(desc.Height)-1-dy; break;
            case DXGI_MODE_ROTATION_ROTATE270: sx=int(desc.Width)-1-dy; sy=dx; break;
            default: break;
            }
            uint32_t pixel=0xff000000;
            if(sx>=0 && sy>=0 && sx<int(desc.Width) && sy<int(desc.Height)) std::memcpy(&pixel,static_cast<BYTE*>(mapped.pData)+size_t(sy)*mapped.RowPitch+sx*4,4);
            p.desktop.pixels[size_t(y)*p.desktop.width+x]=pixel|0xff000000;
        }
        p.context->Unmap(p.staging.get(),0);
    }
    image=p.desktop; if(p.cursor) p.DrawPointer(image); return true;
}
}



