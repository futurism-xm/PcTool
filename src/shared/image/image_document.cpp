#include "shared/image/image_document.h"
#include <limits>
#include <stdexcept>

namespace capture {
Image::Image(int w,int h) : width(w),height(h) {
    if (w<=0 || h<=0 || uint64_t(w)*h > kImageBudget/4)
        throw std::runtime_error("Image exceeds 256 MiB pixel budget");
    pixels.resize(size_t(w)*h,0xff000000);
}
Image Image::Crop(int x,int y,int w,int h) const {
    if (x<0 || y<0 || w<=0 || h<=0 || x>width-w || y>height-h)
        throw std::runtime_error("Invalid image crop");
    Image result(w,h);
    for(int row=0;row<h;++row)
        std::copy_n(pixels.data()+size_t(y+row)*width+x,w,result.pixels.data()+size_t(row)*w);
    return result;
}
}
