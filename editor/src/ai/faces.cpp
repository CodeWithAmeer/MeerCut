#include "fa/ai/faces.h"

#include "fa/ai/models.h"

#include <algorithm>
#include <cmath>

namespace fa::ai {

namespace {

double iou(const Face& a, const Face& b) {
    const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    const double inter = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
    const double uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0;
}

}  // namespace

FaceDetector::FaceDetector(Accelerator acc, int inputWidth) : inputWidth_(inputWidth) {
    const ModelInfo* m = findModel("yunet-face");
    requireModel(*m);
    model_ = std::make_unique<OnnxModel>(modelFilePath(*m, 0), acc);
    // the 2023mar export has a fixed square input (640 x 640): pictures are letterboxed into it
    const auto shape = model_->inputShape(0);
    if (shape.size() == 4 && shape[2] > 0 && shape[3] > 0) inputWidth_ = static_cast<int>(shape[3]);
}

FaceDetector::~FaceDetector() = default;

const std::string& FaceDetector::provider() const { return model_->provider(); }

std::vector<Face> FaceDetector::detect(const Image& pic, double minScore) {
    // letterbox: the long side fills the input, the rest stays black (bottom / right); YuNet takes BGR 0..255
    const int size = inputWidth_;
    const double scale = static_cast<double>(size) / std::max(pic.width, pic.height);
    const int rw = std::max(1, static_cast<int>(std::lround(pic.width * scale)));
    const int rh = std::max(1, static_cast<int>(std::lround(pic.height * scale)));
    Tensor in;
    in.shape = {1, 3, size, size};
    in.data.assign(static_cast<size_t>(3) * size * size, 0.0f);
    const size_t plane = static_cast<size_t>(size) * size;
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            // box filter over the source pixels this input pixel covers (pictures are usually larger than the input)
            const int x0 = static_cast<int>(x / scale), x1 = std::max(x0 + 1, std::min(pic.width, static_cast<int>((x + 1) / scale)));
            const int y0 = static_cast<int>(y / scale), y1 = std::max(y0 + 1, std::min(pic.height, static_cast<int>((y + 1) / scale)));
            int acc[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++n) {
                    const uint8_t* p = &pic.pixels[(static_cast<size_t>(yy) * pic.width + xx) * 4];
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            const size_t i = static_cast<size_t>(y) * size + x;
            in.data[i] = static_cast<float>(acc[2]) / n;               // B
            in.data[plane + i] = static_cast<float>(acc[1]) / n;       // G
            in.data[2 * plane + i] = static_cast<float>(acc[0]) / n;   // R
        }
    const std::vector<std::string> outs = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "bbox_8", "bbox_16", "bbox_32"};
    const auto res = model_->run({{model_->inputNames().front(), &in}}, outs);
    std::vector<Face> faces;
    const int strides[3] = {8, 16, 32};
    for (int s = 0; s < 3; ++s) {
        const int stride = strides[s], cols = size / stride, rows = size / stride;
        const auto& cls = res[static_cast<size_t>(s)].data;
        const auto& obj = res[static_cast<size_t>(3 + s)].data;
        const auto& box = res[static_cast<size_t>(6 + s)].data;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const size_t idx = static_cast<size_t>(r) * cols + c;
                if (idx >= cls.size() || idx * 4 + 3 >= box.size()) continue;
                const double score = std::sqrt(std::clamp<double>(cls[idx], 0, 1) * std::clamp<double>(obj[idx], 0, 1));
                if (score < minScore) continue;
                const double cx = (c + box[idx * 4 + 0]) * stride, cy = (r + box[idx * 4 + 1]) * stride;
                const double bw = std::exp(box[idx * 4 + 2]) * stride, bh = std::exp(box[idx * 4 + 3]) * stride;
                // input pixels -> picture fractions (only the letterboxed area holds the picture)
                Face f{(cx - bw / 2) / rw, (cy - bh / 2) / rh, bw / rw, bh / rh, score};
                if (f.x + f.w / 2 > 1.0 || f.y + f.h / 2 > 1.0) continue;   // centre in the black padding
                faces.push_back(f);
            }
    }
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    std::vector<Face> kept;
    for (const Face& f : faces) {
        if (std::any_of(kept.begin(), kept.end(), [&](const Face& k) { return iou(k, f) > 0.3; })) continue;
        kept.push_back(f);
        if (kept.size() >= 50) break;
    }
    return kept;
}

}  // namespace fa::ai
