#include "fa/vision/stabilize.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cmath>

namespace fa::vision {

struct MotionEstimator::Impl {
    cv::Mat prev;
    double scale = 1;   // analysis picture -> input picture
};

MotionEstimator::MotionEstimator() : impl_(std::make_unique<Impl>()) {}
MotionEstimator::~MotionEstimator() = default;

CameraStep MotionEstimator::next(const Image& frame) {
    Impl& m = *impl_;
    cv::Mat rgba(frame.height, frame.width, CV_8UC4, const_cast<uint8_t*>(frame.pixels.data()), static_cast<size_t>(frame.stride()));
    cv::Mat g, small;
    cv::cvtColor(rgba, g, cv::COLOR_RGBA2GRAY);
    const int w = std::min(frame.width, 960);   // analyse at most 960 px wide
    m.scale = static_cast<double>(frame.width) / w;
    cv::resize(g, small, cv::Size(w, std::max(8, static_cast<int>(std::lround(frame.height / m.scale)))), 0, 0, cv::INTER_AREA);
    CameraStep step;
    if (!m.prev.empty() && m.prev.size() == small.size()) {
        std::vector<cv::Point2f> pts, fwd, back;
        cv::goodFeaturesToTrack(m.prev, pts, 400, 0.01, 12, cv::noArray(), 5);
        if (pts.size() >= 12) {
            std::vector<uchar> s1, s2;
            std::vector<float> err;
            cv::calcOpticalFlowPyrLK(m.prev, small, pts, fwd, s1, err, cv::Size(21, 21), 3);
            cv::calcOpticalFlowPyrLK(small, m.prev, fwd, back, s2, err, cv::Size(21, 21), 3);
            std::vector<cv::Point2f> a, b;
            for (size_t i = 0; i < pts.size(); ++i)
                if (s1[i] && s2[i] && cv::norm(back[i] - pts[i]) < 1.0) {
                    a.push_back(pts[i]);
                    b.push_back(fwd[i]);
                }
            if (a.size() >= 12) {
                std::vector<uchar> inl;
                const cv::Mat T = cv::estimateAffinePartial2D(a, b, inl, cv::RANSAC, 1.0, 2000, 0.99, 10);
                if (!T.empty()) {
                    step.da = std::atan2(T.at<double>(1, 0), T.at<double>(0, 0));
                    step.ds = std::hypot(T.at<double>(0, 0), T.at<double>(1, 0));
                    // express the translation about the picture centre (rotation about the centre, not the corner)
                    const double cx = small.cols / 2.0, cy = small.rows / 2.0;
                    const double rx = T.at<double>(0, 0) * cx + T.at<double>(0, 1) * cy + T.at<double>(0, 2) - cx;
                    const double ry = T.at<double>(1, 0) * cx + T.at<double>(1, 1) * cy + T.at<double>(1, 2) - cy;
                    step.dx = rx * m.scale;
                    step.dy = ry * m.scale;
                    step.valid = true;
                }
            }
        }
    }
    m.prev = small;
    return step;
}

}  // namespace fa::vision
