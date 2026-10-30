// M7 face detection with YuNet (OpenCV Zoo, MIT), run through ONNX Runtime: used by Auto Reframe to find the
// people to keep in frame.
#pragma once

#include "fa/ai/inference.h"
#include "fa/core/image.h"

#include <memory>
#include <vector>

namespace fa::ai {

struct Face {
    double x = 0, y = 0, w = 0, h = 0;   // picture fractions (0..1), top-left + size
    double score = 0;                     // 0..1
};

class FaceDetector {
public:
    // Throws ModelError when the YuNet model is not installed.
    explicit FaceDetector(Accelerator acc = Accelerator::Auto, int inputWidth = 640);
    ~FaceDetector();
    const std::string& provider() const;
    // Faces in a picture, strongest first (score >= minScore, overlapping boxes merged by NMS).
    std::vector<Face> detect(const Image& picture, double minScore = 0.6);

private:
    std::unique_ptr<OnnxModel> model_;
    int inputWidth_;
};

}  // namespace fa::ai
