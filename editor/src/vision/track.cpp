#include "fa/vision/track.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace fa::vision {

namespace {

cv::Mat grey(const Image& img) {
    cv::Mat rgba(img.height, img.width, CV_8UC4, const_cast<uint8_t*>(img.pixels.data()), static_cast<size_t>(img.stride()));
    cv::Mat g;
    cv::cvtColor(rgba, g, cv::COLOR_RGBA2GRAY);
    return g;
}

cv::Rect clampRect(cv::Rect r, const cv::Size& s) { return r & cv::Rect(0, 0, s.width, s.height); }

}  // namespace

struct RegionTracker::Impl {
    cv::Mat prev;                       // previous grey frame
    std::vector<cv::Point2f> points;    // feature points (picture pixels)
    cv::Point2d centre;                 // region centre
    cv::Size2d size, size0;             // current and first size
    cv::Mat templ;                      // appearance at the start (grey, size0)
    double lastConfidence = 1;

    cv::Rect rect() const {
        return cv::Rect(static_cast<int>(std::lround(centre.x - size.width / 2)), static_cast<int>(std::lround(centre.y - size.height / 2)),
                        static_cast<int>(std::lround(size.width)), static_cast<int>(std::lround(size.height)));
    }
    void detect(const cv::Mat& g) {   // features inside the region (with a small inset)
        const cv::Rect r = clampRect(rect(), g.size());
        points.clear();
        if (r.width < 8 || r.height < 8) return;
        cv::Mat mask = cv::Mat::zeros(g.size(), CV_8U);
        mask(r).setTo(255);
        const int want = std::clamp(r.area() / 150, 20, 300);
        cv::goodFeaturesToTrack(g, points, want, 0.01, std::max(3.0, std::min(r.width, r.height) / 25.0), mask, 5);
    }
    double appearance(const cv::Mat& g) const {   // NCC of the template at the current position and size
        const cv::Rect r = clampRect(rect(), g.size());
        if (r.width < 8 || r.height < 8 || templ.empty()) return 0;
        cv::Mat patch, t;
        cv::resize(g(r), patch, templ.size(), 0, 0, cv::INTER_AREA);
        cv::Mat res;
        cv::matchTemplate(patch, templ, res, cv::TM_CCOEFF_NORMED);
        return std::max(0.0, static_cast<double>(res.at<float>(0, 0)));
    }
};

RegionTracker::RegionTracker() : impl_(std::make_unique<Impl>()) {}
RegionTracker::~RegionTracker() = default;

bool RegionTracker::start(const Image& frame, const Box& region) {
    Impl& m = *impl_;
    m.prev = grey(frame);
    m.centre = {region.cx, region.cy};
    m.size = m.size0 = {std::max(8.0, region.w), std::max(8.0, region.h)};
    const cv::Rect r = clampRect(m.rect(), m.prev.size());
    if (r.width < 8 || r.height < 8) return false;
    m.templ = m.prev(r).clone();
    m.detect(m.prev);
    m.lastConfidence = 1;
    return m.points.size() >= 6;
}

TrackPoint RegionTracker::next(const Image& frame) {
    Impl& m = *impl_;
    TrackPoint out;
    const cv::Mat g = grey(frame);
    if (m.points.size() < 6) m.detect(m.prev);
    double inlierShare = 0;
    bool moved = false;
    if (m.points.size() >= 6) {
        std::vector<cv::Point2f> fwd, back;
        std::vector<uchar> st1, st2;
        std::vector<float> err;
        const cv::Size win(21, 21);
        cv::calcOpticalFlowPyrLK(m.prev, g, m.points, fwd, st1, err, win, 3);
        cv::calcOpticalFlowPyrLK(g, m.prev, fwd, back, st2, err, win, 3);
        std::vector<cv::Point2f> from, to;
        for (size_t i = 0; i < m.points.size(); ++i)
            if (st1[i] && st2[i] && cv::norm(back[i] - m.points[i]) < 1.0) {   // forward-backward consistent
                from.push_back(m.points[i]);
                to.push_back(fwd[i]);
            }
        if (from.size() >= 6) {
            std::vector<uchar> inl;
            const cv::Mat T = cv::estimateAffinePartial2D(from, to, inl, cv::RANSAC, 1.5, 2000, 0.99, 10);
            if (!T.empty()) {
                int n = 0;
                for (uchar v : inl) n += v;
                inlierShare = static_cast<double>(n) / static_cast<double>(m.points.size());
                // translation + uniform scale of the region about the transform
                const double a = T.at<double>(0, 0), b = T.at<double>(1, 0);
                const double s = std::clamp(std::sqrt(a * a + b * b), 0.8, 1.25);
                const cv::Point2d c(T.at<double>(0, 0) * m.centre.x + T.at<double>(0, 1) * m.centre.y + T.at<double>(0, 2),
                                    T.at<double>(1, 0) * m.centre.x + T.at<double>(1, 1) * m.centre.y + T.at<double>(1, 2));
                m.centre = c;
                m.size = {m.size.width * s, m.size.height * s};
                std::vector<cv::Point2f> keep;
                for (size_t i = 0; i < to.size(); ++i)
                    if (inl[i]) keep.push_back(to[i]);
                m.points = std::move(keep);
                moved = true;
            }
        }
    }
    double look = m.appearance(g);
    if (!moved || inlierShare < 0.25 || look < 0.35) {   // lost the points or the look: search the template nearby
        const cv::Rect r = m.rect();
        const cv::Rect window = clampRect(cv::Rect(r.x - r.width, r.y - r.height, r.width * 3, r.height * 3), g.size());
        cv::Mat t;
        cv::resize(m.templ, t, cv::Size(std::max(4, r.width), std::max(4, r.height)), 0, 0, cv::INTER_AREA);
        if (window.width > t.cols && window.height > t.rows) {
            cv::Mat res;
            cv::matchTemplate(g(window), t, res, cv::TM_CCOEFF_NORMED);
            double best = 0;
            cv::Point at;
            cv::minMaxLoc(res, nullptr, &best, nullptr, &at);
            if (best > 0.6) {
                m.centre = {window.x + at.x + t.cols / 2.0, window.y + at.y + t.rows / 2.0};
                m.detect(g);
                look = best;
                inlierShare = std::max(inlierShare, 0.5);
                moved = true;
            }
        }
    }
    if (m.points.size() < 12) m.detect(g);   // keep enough points
    m.prev = g;
    out.cx = m.centre.x;
    out.cy = m.centre.y;
    out.scale = m.size.width / m.size0.width;
    out.confidence = std::clamp(0.5 * inlierShare + 0.5 * look, 0.0, 1.0);
    out.lost = !moved || out.confidence < 0.3;
    m.lastConfidence = out.confidence;
    return out;
}

}  // namespace fa::vision
