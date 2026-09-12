#pragma once
#include "shared/image/image_document.h"
#include <optional>
namespace capture {
enum class StitchResult { First, Appended, Unchanged, Reverse, Unmatched, Limit, RegionChanged, Prepended, Revisited };
struct ScrollAnalysis {
    StitchResult result{StitchResult::Unchanged}; RECT region{}; int displacement{};
    double confidence{}; bool locked{}; bool ambiguous{};
    // Body coordinates: the first viewport starts at zero. maxY is exclusive.
    int currentY{},minY{},maxY{},viewportHeight{},upAdded{},totalAdded{};
};
// Pure image analysis. Coordinates always refer to the original capture rectangle.
class ScrollStitcher {
public:
    explicit ScrollStitcher(size_t budget = kImageBudget) : budget_(budget) {}
    StitchResult Push(const Image& frame,std::optional<POINT> anchor = {});
    Image Flatten() const;
    Image Preview(int maxWidth,int maxHeight) const;
    int Height() const { return first_.height+maxY_-minY_-viewportHeight_; }
    int Width() const { return first_.width; }
    const ScrollAnalysis& Analysis()const{return analysis_;}
    double Difference(const Image& a,const Image& b,std::optional<POINT> anchor={})const;
    void MapAnnotations(std::vector<Annotation>& annotations,UINT dpi=96)const;
private:
    uint32_t Pixel(int x,int y)const;
    struct Strip { int begin{}; Image image; };
    Image first_,previous_,footer_;
    std::vector<Strip> strips_; // Nonoverlapping body intervals, sorted by begin.
    std::vector<uint32_t> background_;
    RECT region_{}; int currentY_{},minY_{},maxY_{},viewportHeight_{}; bool locked_{};
    size_t budget_; ScrollAnalysis analysis_;
};
// Independent of the window timer: one completed, stable zero-motion action is
// sufficient, but merely idling or receiving a duplicate observation is not.
class ScrollEndDetector {
public:
    void Reset(){active_=false;direction_=0;time_=0;notified_=false;}
    void Observe(uint64_t id,int64_t time,int direction,StitchResult result,bool stable);
    bool Ready(int64_t now,int64_t stableSince)const;
    void Notified(){notified_=true;}
    int Direction()const{return direction_;} // +1 up, -1 down, 0 none.
private:
    uint64_t lastId_{};int direction_{};int64_t time_{};bool active_{},notified_{};
};
double FrameDifference(const Image& a,const Image& b);
}
