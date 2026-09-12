#include "shared/ui/toolbar_icons.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
namespace capture {
namespace {
// The toolbar icons are embedded as SVG path data so the application has no
// runtime dependency on the source SVG files.
constexpr char kCursorHiddenSvgPath[] = R"svg(M253.6448 221.3888c14.0288-5.12 29.3888-1.8432 48.8448 8.4992 9.9328 5.12 14.848 7.7824 23.552 15.2576l3.7888 3.3792c8.2944 7.8848 13.4144 15.6672 23.6544 31.0272l93.184 139.5712c1.536 2.4576 2.8672 5.12 3.7888 7.9872 3.072 10.24 0 21.0944-6.4512 29.4912a143.36 143.36 0 0 0-18.944 140.6976c9.0112 22.528 9.6256 49.3568-6.144 67.7888-80.7936 94.208-121.344 141.4144-157.696 132.9152a62.464 62.464 0 0 1-14.0288-5.12c-33.1776-17.2032-33.3824-79.36-33.6896-203.4688L212.992 393.4208c-0.2048-98.5088-0.2048-147.968 28.3648-166.0928a61.7472 61.7472 0 0 1 12.288-5.9392z m489.472 64.7168a51.2 51.2 0 0 1 72.3968 72.3968l-33.28 33.3824c-18.432 18.3296-27.5456 27.4432-28.3648 38.7072a31.0272 31.0272 0 0 0 0 4.8128c0.8192 11.264 10.0352 20.48 28.3648 38.912l33.28 33.1776a51.2 51.2 0 0 1-72.3968 72.3968l-33.28-33.1776c-18.432-18.3296-27.5456-27.4432-38.912-28.3648a30.9248 30.9248 0 0 0-4.7104 0c-11.264 0.9216-20.48 10.0352-38.912 28.3648v0.1024l-33.1776 33.1776-7.9872 6.656a50.5856 50.5856 0 0 1 7.9872-6.656 51.2 51.2 0 0 1-72.3968-72.4992l33.1776-33.1776c18.432-18.432 27.648-27.648 28.4672-38.912a31.1296 31.1296 0 0 0 0-4.8128c-0.8192-11.264-10.1376-20.48-28.4672-38.7072L521.728 358.4a51.2 51.2 0 0 1 72.3968-72.3968l33.1776 33.3824c18.432 18.3296 27.648 27.4432 38.912 28.3648a31.0272 31.0272 0 0 0 4.8128 0c11.264-0.9216 20.48-10.0352 38.8096-28.3648l33.28-33.3824z)svg";
constexpr char kCursorSvgPath[] = R"svg(M334.4384 222.3104c31.9488-11.264 70.7584 19.1488 148.2752 80.0768L632.4224 419.84c99.2256 77.824 148.8896 116.8384 141.6192 153.8048a61.44 61.44 0 0 1-4.608 14.1312c-15.9744 34.0992-79.0528 36.2496-205.1072 40.5504a56.2176 56.2176 0 0 0-40.1408 18.8416l-25.8048 29.0816c-80.5888 90.624-120.9344 135.8848-156.8768 126.976a61.6448 61.6448 0 0 1-13.824-5.2224c-32.768-17.2032-32.9728-77.824-33.28-199.0656L293.888 394.4448c-0.2048-98.6112-0.3072-147.968 28.2624-166.0928a61.44 61.44 0 0 1 12.288-6.0416z)svg";
constexpr char kLaserSvgPath[] = R"svg(M745.8 618.7l122.1-70.5c9.5-5.5 15.1-16 14.3-27-0.8-11-7.8-20.6-18-24.6L319.7 279.4c-10.7-4.3-22.9-1.8-31 6.4-8.1 8.1-10.6 20.3-6.4 31l217.2 544.5c4.1 10.2 13.6 17.2 24.6 18 11 0.8 21.5-4.8 27-14.3l70.5-122.1L792.7 914c5.4 5.4 12.7 8.4 20.3 8.4 7.6 0 14.9-3 20.3-8.4l83.5-83.5c11.2-11.2 11.2-29.4 0-40.7l-171-171.1zM813.1 853L635.8 675.7c-5.4-5.4-12.8-8.4-20.3-8.4-1.3 0-2.5 0.1-3.8 0.2-8.9 1.2-16.7 6.4-21.2 14.1l-59.6 103.2-170.4-427.2L787.8 528l-103.2 59.6c-7.7 4.5-13 12.3-14.1 21.1-1.2 8.9 1.9 17.8 8.2 24.1L856 810.1 813.1 853zM214.9 252.6c5.6 5.6 13 8.4 20.3 8.4 7.4 0 14.7-2.8 20.3-8.4 11.2-11.2 11.2-29.4 0-40.7L191.6 148c-11.2-11.2-29.4-11.2-40.7 0-11.2 11.2-11.2 29.5 0 40.7l64 63.9z m-120.7 123h90.4c15.9 0 28.8-12.9 28.8-28.8s-12.9-28.7-28.8-28.7H94.2c-15.9 0-28.8 12.9-28.8 28.7 0 15.9 12.9 28.8 28.8 28.8z m43.5 123.9c-11.2 11.2-11.2 29.4 0 40.7 5.6 5.6 13 8.4 20.3 8.4 7.4 0 14.7-2.8 20.3-8.4l63.9-63.9c11.2-11.2 11.2-29.4 0-40.7-11.2-11.2-29.4-11.2-40.7 0l-63.8 63.9zM321 91.2v90.4c0 15.9 12.9 28.8 28.8 28.8s28.8-12.9 28.8-28.8V91.2c0-15.9-12.9-28.8-28.8-28.8-15.9 0.1-28.8 12.9-28.8 28.8z m158.2 148.2l63.9-63.9c11.2-11.2 11.2-29.4 0-40.7-11.2-11.2-29.4-11.2-40.7 0l-63.9 63.9c-11.2 11.2-11.2 29.4 0 40.7 5.6 5.6 13 8.4 20.3 8.4 7.4 0 14.8-2.8 20.4-8.4z)svg";
constexpr char kClearSvgPath[] = R"svg(M836.6 193.8h100.2c18.8 0 33.9-16.2 33.9-36.3s-15.1-35.2-33.9-35.2H724.9V85.9C724.9 40 690 2 646.6 2H377.9c-42.8 0-78.3 37.5-78.3 83.9v36.3H87.2c-18.8 0-33.9 16.2-33.9 36.3s15.1 36.3 33.9 36.3h100.2l649.2-1zM804.6 253.7l-585.2 1c-20.5 0-37.2 17.6-37.2 39.2V890c0 72.6 56.3 132 125.1 132h409.4c68.8 0 125.1-59.4 125.1-132V293c0-21.7-16.7-39.3-37.2-39.3z)svg";
constexpr char kGifSvgPath[] = R"svg(M928 640h32V160h-32v-27.072C928 77.28 883.552 32 828.928 32H195.04C140.448 32 96 77.28 96 132.928V160H64v480h32v251.072C96 946.72 140.448 992 195.04 992h480.032a31.872 31.872 0 0 0 20.96-7.84l153.856-133.632a32 32 0 1 0-41.92-48.32L704 892.512v-119.552c0-20.384 15.712-36.96 35.072-36.96h157.632a32 32 0 0 0 32-32c0-1.792-0.736-3.36-1.024-5.088 0-0.576 0.32-1.056 0.32-1.664V640z m-64 32h-124.928C684.448 672 640 717.28 640 772.928V928H195.04C175.712 928 160 911.424 160 891.072V640h704v32zM319.872 466.72c7.968 2.88 15.072 4.32 21.344 4.32h5.792c11.808 0 22.784-3.744 32.928-11.2v-14.848H334.72c-10.368 0-18.56-7.584-24.608-22.784v-9.024c0-7.488 4.704-14.592 14.112-21.344 4.096-2.912 11.456-4.352 22.08-4.352h54.976c19.04 0 30.88 6.88 35.456 20.608l0.736 5.056v63.296c0 13.024-16.032 27.488-48.128 43.424-16.416 5.536-29.44 8.32-39.072 8.32h-11.936c-31.104 0-60.064-14.336-86.816-43.04-18.368-24.128-27.52-47.872-27.52-71.264v-11.584c0-31.104 14.336-60.032 43.04-86.816C291.168 297.152 314.912 288 338.304 288h11.936c19.52 0 40.992 7.488 64.384 22.432 15.2 10.624 22.784 20.992 22.784 31.104v5.056c0 4.576-2.88 10.496-8.672 17.728-7.232 5.056-13.76 7.616-19.552 7.616-8.448 0-17.728-4.96-27.84-14.848-11.104-7.968-22.56-11.936-34.368-11.936h-5.792c-23.872 0-42.432 13.024-55.712 39.072a60.8 60.8 0 0 0-4.352 20.992v5.792c0.032 23.872 12.928 42.432 38.752 55.712z m185.632 39.072V312.608c0-10.624 7.488-18.816 22.432-24.608h9.408c10.624 0 18.816 7.488 24.608 22.432v193.184c0 10.624-7.488 18.816-22.432 24.608h-9.408c-10.624 0-18.848-7.488-24.608-22.432z m188.448-115.424v10.496h73.44c10.624 0 18.944 7.616 24.96 22.784v9.056c0 7.488-4.832 14.592-14.464 21.344-3.872 2.88-11.104 4.352-21.696 4.352h-62.208v32.192c0 19.296-5.664 30.88-16.992 34.72 0 1.216-2.784 2.176-8.32 2.912h-7.232c-10.624 0-18.816-7.488-24.608-22.432V312.608c0-10.624 7.488-18.816 22.432-24.608h122.272c10.624 0 18.944 7.488 24.96 22.432v9.408c0 10.624-7.584 18.816-22.784 24.608h-90.432l0.672 45.92zM864 160H160v-27.072C160 112.576 175.712 96 195.04 96h633.888C848.288 96 864 112.576 864 132.928V160z)svg";
constexpr char kPinSvgPath[] = R"svg(M64.24521 957.421652l354.369913-280.94667 141.369751 150.35745s59.013045 16.08228 62.644759-28.581966l-3.631714-152.20554 209.43087-241.588268 91.224678-5.296632s71.576175-14.410198 19.710924-76.994582L683.425278 64.49899s-69.731154-8.933463-64.431452 55.43966v69.791529L381.039319 392.074653l-146.725736 8.872065s-53.712319 17.86488-34.063816 64.431452l137.853671 136.009673L64.24214 957.422675l0.00307-0.001023z)svg";
constexpr char kMosaicSvgPath[] = R"svg(M725.333333 78.933333A219.733333 219.733333 0 0 1 945.066667 298.666667v426.666666A219.733333 219.733333 0 0 1 725.333333 945.066667H298.666667A219.733333 219.733333 0 0 1 78.933333 725.333333V298.666667A219.733333 219.733333 0 0 1 298.666667 78.933333h426.666666z m-213.333333 98.090667L298.666667 177.066667A121.6 121.6 0 0 0 177.066667 298.666667l-0.042667 213.333333H512l-0.042667 334.933333H725.333333A121.6 121.6 0 0 0 846.933333 725.333333v-213.333333H512V177.024z)svg";
constexpr char kPenSvgPath[] = R"svg(M745.76 369.86l-451 537.48a18.693 18.693 0 0 1-8.46 5.74l-136.58 45.27c-13.24 4.39-26.46-6.71-24.43-20.5l20.86-142.36c0.5-3.44 1.95-6.67 4.19-9.33l451-537.48c6.65-7.93 18.47-8.96 26.4-2.31l115.71 97.1c7.92 6.64 8.96 18.46 2.31 26.39zM894.53 192.56l-65.9 78.53c-6.65 7.93-18.47 8.96-26.4 2.31l-115.71-97.1c-7.93-6.65-8.96-18.47-2.31-26.4l65.9-78.53c6.65-7.93 18.47-8.96 26.4-2.31l115.71 97.1c7.93 6.65 8.96 18.47 2.31 26.4z)svg";
constexpr char kTextSvgPath[] = R"svg(M379.733333 635.733333l145.066667-384 145.066667 384M465.066667 85.333333L128 938.666667h136.533333l68.266667-183.466667h379.733333l68.266667 183.466667h136.533333L584.533333 85.333333h-119.466666z)svg";
constexpr char kRectangleSvgPath[] = R"svg(M841.34 959.36H182.66c-65.06 0-117.99-52.94-117.99-118.02V182.69c0-65.08 52.94-118.04 117.99-118.04h658.68c65.06 0 117.99 52.96 117.99 118.04v658.65c0 65.08-52.93 118.02-117.99 118.02zM182.66 142.17c-22.31 0-40.51 18.18-40.51 40.51v658.65c0 22.34 18.2 40.49 40.51 40.49h658.68c22.31 0 40.51-18.15 40.51-40.49V182.69c0-22.34-18.2-40.51-40.51-40.51H182.66z)svg";
constexpr char kEllipseSvgPath[] = R"svg(M512.5 146c47.9 0 94.3 9.4 137.9 27.8 42.2 17.8 80.1 43.4 112.7 76 32.6 32.6 58.2 70.5 76 112.7 18.5 43.6 27.8 90 27.8 138s-9.4 94.3-27.8 138c-17.8 42.2-43.4 80.1-76 112.7-32.6 32.6-70.5 58.2-112.7 76-43.6 18.5-90 27.8-137.9 27.8-47.9 0-94.3-9.4-138-27.8-42.2-17.8-80.1-43.4-112.7-76-32.6-32.6-58.2-70.5-76-112.7-18.5-43.6-27.8-90-27.8-138s9.4-94.3 27.8-138c17.8-42.2 43.4-80.1 76-112.7 32.6-32.6 70.5-58.2 112.7-76 43.7-18.4 90.1-27.8 138-27.8m0-120C250.4 26 38 238.4 38 500.5S250.4 975 512.5 975 987 762.6 987 500.5 774.6 26 512.5 26z)svg";
constexpr char kArrowSvgPath[] = R"svg(M139.08607033 853.67240896l468.62281276-468.62281277L651.09988438 428.44059749 182.47707162 897.06341025l-43.39100128-43.3910013zM475.22620999 257.84308963L875.75699299 153.70508632l-100.13269568 404.53609063-300.39808732-300.39808732z)svg";
constexpr char kNumberSvgPath[] = R"svg(M512 0C229.696 0 0 229.632 0 511.872s229.696 511.936 512 511.936 512-229.632 512-511.936C1024 229.632 794.304 0 512 0z m0 950.656c-241.984 0-438.848-196.8-438.848-438.784 0-241.92 196.864-438.72 438.848-438.72s438.848 196.8 438.848 438.72c0 241.92-196.864 438.784-438.848 438.784z m-101.952-564.48l93.312-46.72v428.416h73.152V221.184L377.28 320.768l32.768 65.408z)svg";
constexpr char kCancelSvgPath[] = R"svg(M577.697 484.155L1010.11 73.753A42.742 42.742 0 0 0 1008 13.16a48.373 48.373 0 0 0-63.73-1.92L512.048 421.77 79.571 11.305a48.373 48.373 0 0 0-63.73 1.92 42.742 42.742 0 0 0-2.111 60.53l432.413 410.401L13.73 894.622a42.55 42.55 0 0 0-10.174 48.245 46.453 46.453 0 0 0 43.126 27.13 47.67 47.67 0 0 0 32.889-12.926L511.984 546.67l432.35 410.402c8.701 8.318 20.538 13.053 32.952 13.053a47.605 47.605 0 0 0 32.888-13.053 42.998 42.998 0 0 0 13.629-31.225 42.998 42.998 0 0 0-13.629-31.224L577.633 484.155h0.064z)svg";
constexpr char kFinishSvgPath[] = R"svg(M899.418 297.916l-78.324-78.324-430.119 431.116-188.631-188.093-78.324 78.324 267.805 266.066 77.676-78-0.09-0.089 430.01-430.999zM899.418 297.916z)svg";

class SvgPathReader {
public:
    explicit SvgPathReader(const char* source) : cursor_(source) {}

    bool Read(Gdiplus::GraphicsPath& path) {
        path.SetFillMode(Gdiplus::FillModeWinding);
        char command = 0;
        while (true) {
            SkipSeparators();
            if (*cursor_ == '\0') return true;
            if (std::isalpha(static_cast<unsigned char>(*cursor_))) {
                command = *cursor_++;
            } else if (command == 0) {
                return false;
            }
            if (!ReadCommand(path, command)) return false;
        }
    }

private:
    void SkipSeparators() {
        while (*cursor_ != '\0' &&
            (std::isspace(static_cast<unsigned char>(*cursor_)) || *cursor_ == ',')) {
            ++cursor_;
        }
    }

    bool HasNumber() {
        SkipSeparators();
        return *cursor_ == '+' || *cursor_ == '-' || *cursor_ == '.' ||
            std::isdigit(static_cast<unsigned char>(*cursor_));
    }

    bool Number(double& value) {
        SkipSeparators();
        char* end = nullptr;
        value = std::strtod(cursor_, &end);
        if (end == cursor_) return false;
        cursor_ = end;
        return true;
    }

    static Gdiplus::PointF Point(double x, double y) {
        return {static_cast<float>(x), static_cast<float>(y)};
    }

    void AddArc(Gdiplus::GraphicsPath& path, double rx, double ry,
        double rotationDegrees, bool largeArc, bool sweep, double x, double y) {
        const double x1 = currentX_;
        const double y1 = currentY_;
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0.0 || ry == 0.0 || (x1 == x && y1 == y)) {
            path.AddLine(Point(x1, y1), Point(x, y));
            currentX_ = x;
            currentY_ = y;
            return;
        }

        constexpr double pi = 3.14159265358979323846;
        const double phi = rotationDegrees * pi / 180.0;
        const double cosPhi = std::cos(phi);
        const double sinPhi = std::sin(phi);
        const double dx = (x1 - x) / 2.0;
        const double dy = (y1 - y) / 2.0;
        const double xPrime = cosPhi * dx + sinPhi * dy;
        const double yPrime = -sinPhi * dx + cosPhi * dy;
        const double radiusScale = xPrime * xPrime / (rx * rx) +
            yPrime * yPrime / (ry * ry);
        if (radiusScale > 1.0) {
            const double scale = std::sqrt(radiusScale);
            rx *= scale;
            ry *= scale;
        }

        const double rxSquared = rx * rx;
        const double rySquared = ry * ry;
        const double numerator = std::max(0.0,
            rxSquared * rySquared - rxSquared * yPrime * yPrime -
            rySquared * xPrime * xPrime);
        const double denominator = rxSquared * yPrime * yPrime +
            rySquared * xPrime * xPrime;
        double coefficient = denominator == 0.0 ? 0.0 :
            std::sqrt(numerator / denominator);
        if (largeArc == sweep) coefficient = -coefficient;
        const double centerPrimeX = coefficient * rx * yPrime / ry;
        const double centerPrimeY = coefficient * -ry * xPrime / rx;
        const double centerX = cosPhi * centerPrimeX - sinPhi * centerPrimeY +
            (x1 + x) / 2.0;
        const double centerY = sinPhi * centerPrimeX + cosPhi * centerPrimeY +
            (y1 + y) / 2.0;

        auto vectorAngle = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double ux = (xPrime - centerPrimeX) / rx;
        const double uy = (yPrime - centerPrimeY) / ry;
        const double vx = (-xPrime - centerPrimeX) / rx;
        const double vy = (-yPrime - centerPrimeY) / ry;
        double startAngle = std::atan2(uy, ux);
        double deltaAngle = vectorAngle(ux, uy, vx, vy);
        if (!sweep && deltaAngle > 0.0) deltaAngle -= 2.0 * pi;
        if (sweep && deltaAngle < 0.0) deltaAngle += 2.0 * pi;
        const int segments = std::max(1,
            static_cast<int>(std::ceil(std::abs(deltaAngle) / (pi / 2.0))));
        const double step = deltaAngle / segments;

        auto mapPoint = [&](double unitX, double unitY) {
            return Point(centerX + rx * cosPhi * unitX - ry * sinPhi * unitY,
                centerY + rx * sinPhi * unitX + ry * cosPhi * unitY);
        };
        for (int segment = 0; segment < segments; ++segment) {
            const double firstAngle = startAngle + segment * step;
            const double secondAngle = firstAngle + step;
            const double alpha = 4.0 / 3.0 * std::tan(step / 4.0);
            const double cosFirst = std::cos(firstAngle);
            const double sinFirst = std::sin(firstAngle);
            const double cosSecond = std::cos(secondAngle);
            const double sinSecond = std::sin(secondAngle);
            const Gdiplus::PointF start = mapPoint(cosFirst, sinFirst);
            const Gdiplus::PointF control1 =
                mapPoint(cosFirst - alpha * sinFirst,
                    sinFirst + alpha * cosFirst);
            const Gdiplus::PointF control2 =
                mapPoint(cosSecond + alpha * sinSecond,
                    sinSecond - alpha * cosSecond);
            const Gdiplus::PointF end = mapPoint(cosSecond, sinSecond);
            path.AddBezier(start, control1, control2, end);
        }
        currentX_ = x;
        currentY_ = y;
    }

    bool ReadCommand(Gdiplus::GraphicsPath& path, char command) {
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        if (upper == 'Z') {
            path.CloseFigure();
            currentX_ = figureStartX_;
            currentY_ = figureStartY_;
            lastCommand_ = upper;
            return true;
        }

        bool first = true;
        while (HasNumber()) {
            const double baseX = relative ? currentX_ : 0.0;
            const double baseY = relative ? currentY_ : 0.0;
            double a = 0.0, b = 0.0, c = 0.0, d = 0.0, e = 0.0, f = 0.0, g = 0.0;
            if (upper == 'M' || upper == 'L') {
                if (!Number(a) || !Number(b)) return false;
                const double x = baseX + a;
                const double y = baseY + b;
                if (upper == 'M' && first) {
                    path.StartFigure();
                    figureStartX_ = x;
                    figureStartY_ = y;
                } else {
                    path.AddLine(Point(currentX_, currentY_), Point(x, y));
                }
                currentX_ = x; currentY_ = y;
            } else if (upper == 'H') {
                if (!Number(a)) return false;
                const double x = baseX + a;
                path.AddLine(Point(currentX_, currentY_), Point(x, currentY_));
                currentX_ = x;
            } else if (upper == 'V') {
                if (!Number(a)) return false;
                const double y = baseY + a;
                path.AddLine(Point(currentX_, currentY_), Point(currentX_, y));
                currentY_ = y;
            } else if (upper == 'C') {
                if (!Number(a) || !Number(b) || !Number(c) || !Number(d) ||
                    !Number(e) || !Number(f)) return false;
                controlX_ = baseX + c; controlY_ = baseY + d;
                const double x = baseX + e, y = baseY + f;
                path.AddBezier(Point(currentX_, currentY_), Point(baseX + a, baseY + b),
                    Point(controlX_, controlY_), Point(x, y));
                currentX_ = x; currentY_ = y;
            } else if (upper == 'S') {
                if (!Number(a) || !Number(b) || !Number(c) || !Number(d)) return false;
                const double firstControlX =
                    (lastCommand_ == 'C' || lastCommand_ == 'S') ? 2.0 * currentX_ - controlX_ : currentX_;
                const double firstControlY =
                    (lastCommand_ == 'C' || lastCommand_ == 'S') ? 2.0 * currentY_ - controlY_ : currentY_;
                controlX_ = baseX + a; controlY_ = baseY + b;
                const double x = baseX + c, y = baseY + d;
                path.AddBezier(Point(currentX_, currentY_), Point(firstControlX, firstControlY),
                    Point(controlX_, controlY_), Point(x, y));
                currentX_ = x; currentY_ = y;
            } else if (upper == 'A') {
                if (!Number(a) || !Number(b) || !Number(c) || !Number(d) ||
                    !Number(e) || !Number(f) || !Number(g)) return false;
                AddArc(path, a, b, c, d != 0.0, e != 0.0, baseX + f, baseY + g);
            } else {
                return false;
            }
            first = false;
            lastCommand_ = upper;
            if (upper == 'M') command = relative ? 'l' : 'L';
        }
        return !first;
    }

    const char* cursor_{};
    double currentX_{};
    double currentY_{};
    double figureStartX_{};
    double figureStartY_{};
    double controlX_{};
    double controlY_{};
    char lastCommand_{};
};

bool FillEmbeddedSvgPath(Gdiplus::Graphics& graphics, const char* svgPath,
    const RECT& buttonRect, float size, const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path(Gdiplus::FillModeWinding);
    SvgPathReader reader(svgPath);
    if (!reader.Read(path) || path.GetPointCount() == 0) return false;
    Gdiplus::RectF bounds;
    if (path.GetBounds(&bounds) != Gdiplus::Ok ||
        bounds.Width <= 0.0F || bounds.Height <= 0.0F) return false;
    const float scale = size / std::max(bounds.Width, bounds.Height);
    const float centerX = (buttonRect.left + buttonRect.right) / 2.0F;
    const float centerY = (buttonRect.top + buttonRect.bottom) / 2.0F;
    Gdiplus::Matrix transform(scale, 0.0F, 0.0F, scale,
        centerX - (bounds.X + bounds.Width / 2.0F) * scale,
        centerY - (bounds.Y + bounds.Height / 2.0F) * scale);
    path.Transform(&transform);
    Gdiplus::SolidBrush brush(color);
    return graphics.FillPath(&brush, &path) == Gdiplus::Ok;
}

int RectWidth(const RECT& r) { return r.right-r.left; }
int RectHeight(const RECT& r) { return r.bottom-r.top; }
}
bool DrawSvgIcon(HDC dc,const char* path,const RECT& rect,COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    return FillEmbeddedSvgPath(graphics,path,rect,float(std::min(rect.right-rect.left,rect.bottom-rect.top)),
        Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
}
void DrawToolbarIcon(HDC deviceContext,ToolbarIcon icon,const RECT& rect,UINT dpi,COLORREF color,float sizeDip) {
    const float dpiScale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
    const float centerX = (rect.left + rect.right) / 2.0F;
    const float centerY = (rect.top + rect.bottom) / 2.0F;
    const float radius = sizeDip * 0.5F * dpiScale;
    const float strokeWidth = std::max(1.45F, 1.6F * dpiScale);
    const Gdiplus::Color vectorColor(255, GetRValue(color),
        GetGValue(color), GetBValue(color));
    Gdiplus::Graphics graphics(deviceContext);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Pen pen(vectorColor, strokeWidth);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    auto Scale=[dpi](int value){return MulDiv(value,int(dpi),96);};
    switch (icon) {
    case ToolbarIcon::Grip: {
        Gdiplus::SolidBrush dot(vectorColor);
        const float dotRadius = std::max(1.15F, 1.15F * dpiScale);
        for (int index = -1; index <= 1; ++index) {
            const float y = centerY + index * 5.0F * dpiScale;
            graphics.FillEllipse(&dot, centerX - dotRadius, y - dotRadius,
                dotRadius * 2.0F, dotRadius * 2.0F);
        }
        break;
    }
    case ToolbarIcon::LongCapture: {
        const float r=radius;
        const Gdiplus::PointF paper[]={{centerX-r*0.8F,centerY-r*0.1F},
            {centerX-r*0.8F,centerY-r},{centerX+r*0.8F,centerY-r},{centerX+r*0.8F,centerY-r*0.1F}};
        graphics.DrawLines(&pen,paper,4);
        graphics.DrawLine(&pen,centerX-r*0.28F,centerY+r*0.35F,centerX+r*0.4F,centerY-r*0.45F);
        graphics.DrawLine(&pen,centerX+r*0.28F,centerY+r*0.35F,centerX-r*0.4F,centerY-r*0.45F);
        graphics.DrawEllipse(&pen,centerX-r*0.69F,centerY+r*0.34F,r*0.48F,r*0.48F);
        graphics.DrawEllipse(&pen,centerX+r*0.21F,centerY+r*0.34F,r*0.48F,r*0.48F);
        break;
    }
    case ToolbarIcon::Ocr: {
        const float r=radius;
        for(float x:{-1.0F,1.0F}) for(float y:{-1.0F,1.0F}) {
            const Gdiplus::PointF corner[]={{centerX+x*r*0.55F,centerY+y*r},
                {centerX+x*r,centerY+y*r},{centerX+x*r,centerY+y*r*0.55F}};
            graphics.DrawLines(&pen,corner,3);
        }
        // Center the actual glyph outline, avoiding font side bearings and
        // keeping the character sharp at the toolbar's native DPI.
        Gdiplus::FontFamily family(L"Microsoft YaHei UI");
        Gdiplus::GraphicsPath character;
        character.AddString(L"文",1,&family,Gdiplus::FontStyleRegular,r*1.55F,Gdiplus::PointF(0,0),nullptr);
        Gdiplus::RectF bounds; character.GetBounds(&bounds);
        Gdiplus::Matrix placement;
        placement.Translate(centerX-bounds.X-bounds.Width/2,centerY-bounds.Y-bounds.Height/2);
        character.Transform(&placement);
        Gdiplus::SolidBrush brush(vectorColor); graphics.FillPath(&brush,&character);
        break;
    }
    case ToolbarIcon::Record: {
        graphics.DrawEllipse(&pen,centerX-radius,centerY-radius,radius*2,radius*2);
        Gdiplus::SolidBrush brush(vectorColor);
        graphics.FillEllipse(&brush,centerX-radius*0.55F,centerY-radius*0.55F,radius*1.1F,radius*1.1F);
        break;
    }
    case ToolbarIcon::Rectangle:
        FillEmbeddedSvgPath(graphics, kRectangleSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    case ToolbarIcon::Ellipse:
        FillEmbeddedSvgPath(graphics, kEllipseSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    case ToolbarIcon::Arrow:
        FillEmbeddedSvgPath(graphics, kArrowSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    case ToolbarIcon::Pen: {
        FillEmbeddedSvgPath(graphics, kPenSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Mosaic: {
        FillEmbeddedSvgPath(graphics, kMosaicSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Text: {
        FillEmbeddedSvgPath(graphics, kTextSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Number: {
        FillEmbeddedSvgPath(graphics, kNumberSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Undo: {
        Gdiplus::FontFamily fontFamily(L"Segoe UI Symbol");
        Gdiplus::Font font(&fontFamily, 19.0F * dpiScale,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(vectorColor);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const Gdiplus::RectF symbolRect(static_cast<float>(rect.left),
            static_cast<float>(rect.top), static_cast<float>(RectWidth(rect)),
            static_cast<float>(RectHeight(rect)));
        graphics.DrawString(L"\u21A9", 1, &font,
            symbolRect, &format, &textBrush);
        break;
    }
    case ToolbarIcon::Gif: {
        FillEmbeddedSvgPath(graphics, kGifSvgPath, rect, radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Pin: {
        FillEmbeddedSvgPath(graphics, kPinSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Save:
        graphics.DrawLine(&pen, centerX, centerY - radius,
            centerX, centerY + 3.0F * dpiScale);
        graphics.DrawLine(&pen, centerX, centerY + 3.0F * dpiScale,
            centerX - 4.5F * dpiScale, centerY - 1.5F * dpiScale);
        graphics.DrawLine(&pen, centerX, centerY + 3.0F * dpiScale,
            centerX + 4.5F * dpiScale, centerY - 1.5F * dpiScale);
        graphics.DrawLine(&pen, centerX - radius, centerY + radius,
            centerX + radius, centerY + radius);
        graphics.DrawLine(&pen, centerX - radius, centerY + radius,
            centerX - radius, centerY + 5.0F * dpiScale);
        graphics.DrawLine(&pen, centerX + radius, centerY + radius,
            centerX + radius, centerY + 5.0F * dpiScale);
        break;
    case ToolbarIcon::Cancel:
        FillEmbeddedSvgPath(graphics, kCancelSvgPath, rect,
            radius * 2.0F, vectorColor);
        break;
    case ToolbarIcon::Finish: {
        const RECT checkRect{rect.left + Scale(8), rect.top,
            rect.left + Scale(24), rect.bottom};
        FillEmbeddedSvgPath(graphics, kFinishSvgPath, checkRect,
            radius * 2.0F, vectorColor);

        Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
        Gdiplus::Font font(&fontFamily, 12.0F * dpiScale,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(vectorColor);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        const Gdiplus::RectF textRect(
            rect.left + 30.0F * dpiScale,
            static_cast<float>(rect.top),
            std::max(0.0F, rect.right - rect.left - 33.0F * dpiScale),
            static_cast<float>(RectHeight(rect)));
        graphics.DrawString(L"完成", 2, &font, textRect, &format, &textBrush);
        break;
    }
    case ToolbarIcon::System:
    case ToolbarIcon::SystemMuted: {
        Gdiplus::PointF points[]={{centerX-8*dpiScale,centerY-3*dpiScale},{centerX-4*dpiScale,centerY-3*dpiScale},{centerX+1*dpiScale,centerY-8*dpiScale},{centerX+1*dpiScale,centerY+8*dpiScale},{centerX-4*dpiScale,centerY+3*dpiScale},{centerX-8*dpiScale,centerY+3*dpiScale}};
        graphics.DrawPolygon(&pen,points,6);
        graphics.DrawArc(&pen,centerX-1*dpiScale,centerY-5*dpiScale,8*dpiScale,10*dpiScale,-55.0F,110.0F);
        graphics.DrawArc(&pen,centerX-2*dpiScale,centerY-8*dpiScale,13*dpiScale,16*dpiScale,-55.0F,110.0F);
        break;
    }
    case ToolbarIcon::Mic:
    case ToolbarIcon::MicMuted: {
        Gdiplus::GraphicsPath capsule;
        capsule.AddArc(centerX-3*dpiScale,centerY-9*dpiScale,6*dpiScale,6*dpiScale,180.0F,180.0F);
        capsule.AddArc(centerX-3*dpiScale,centerY-2*dpiScale,6*dpiScale,6*dpiScale,0.0F,180.0F); capsule.CloseFigure();
        graphics.DrawPath(&pen,&capsule);
        graphics.DrawArc(&pen,centerX-6*dpiScale,centerY-4*dpiScale,12*dpiScale,12*dpiScale,0.0F,180.0F);
        graphics.DrawLine(&pen,centerX-6*dpiScale,centerY-2*dpiScale,centerX-6*dpiScale,centerY+2*dpiScale);
        graphics.DrawLine(&pen,centerX+6*dpiScale,centerY-2*dpiScale,centerX+6*dpiScale,centerY+2*dpiScale);
        graphics.DrawLine(&pen,centerX,centerY+8*dpiScale,centerX,centerY+11*dpiScale);
        graphics.DrawLine(&pen,centerX-4*dpiScale,centerY+11*dpiScale,centerX+4*dpiScale,centerY+11*dpiScale);
        break;
    }
    case ToolbarIcon::CursorHidden: { FillEmbeddedSvgPath(graphics, kCursorHiddenSvgPath, rect, radius * 2.0F, vectorColor); break; }
    case ToolbarIcon::Cursor: { FillEmbeddedSvgPath(graphics, kCursorSvgPath, rect, radius * 2.0F, vectorColor); break; }
    case ToolbarIcon::Laser: {
        FillEmbeddedSvgPath(graphics, kLaserSvgPath, rect, radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Clear: {
        FillEmbeddedSvgPath(graphics, kClearSvgPath, rect, radius * 2.0F, vectorColor);
        break;
    }
    case ToolbarIcon::Pause:
        graphics.DrawLine(&pen,centerX-3*dpiScale,centerY-7*dpiScale,centerX-3*dpiScale,centerY+7*dpiScale);
        graphics.DrawLine(&pen,centerX+3*dpiScale,centerY-7*dpiScale,centerX+3*dpiScale,centerY+7*dpiScale); break;
    case ToolbarIcon::Resume: {
        const Gdiplus::PointF triangle[]={{centerX-4*dpiScale,centerY-7*dpiScale},{centerX+7*dpiScale,centerY},{centerX-4*dpiScale,centerY+7*dpiScale}};
        graphics.DrawPolygon(&pen,triangle,3); break;
    }
    case ToolbarIcon::Style: {
        Gdiplus::SolidBrush brush(vectorColor); graphics.FillRectangle(&brush,centerX-radius,centerY-radius,radius*2,radius*2);
        Gdiplus::Pen border(Gdiplus::Color(255,60,72,90),strokeWidth); graphics.DrawRectangle(&border,centerX-radius,centerY-radius,radius*2,radius*2); break;
    }
    case ToolbarIcon::None:
        break;
    }
    if(icon==ToolbarIcon::SystemMuted || icon==ToolbarIcon::MicMuted)
        graphics.DrawLine(&pen,centerX-9*dpiScale,centerY+10*dpiScale,centerX+9*dpiScale,centerY-10*dpiScale);

}

}
