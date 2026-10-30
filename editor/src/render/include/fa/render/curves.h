// Colour curves (M4): smooth curves through control points, built like Filmora's (Stage 1 EXP-019, render matched
// to 0.55 levels RMS): cubic Bezier segments with handles at 1/3 and 2/3 of each segment in x; the tangent at an
// interior point is the slope between its two neighbours; at an end point it is the "natural" end condition that
// puts the end handle half-way between the end point and the next segment handle.
//   tone curves:     x, y in 0..1, held flat beyond the first / last point; identity = (0,0) (1,1)
//   periodic curves: hue curves; x in 0..1 wraps around (hue/360); y = 0.5 is neutral; no points = neutral
#pragma once

#include "fa/anim/animatable.h"

#include <vector>

namespace fa {

double evaluateCurve(std::vector<Vec2> points, double x, bool periodic);
// n samples at x = i / (n - 1), clamped to 0..1.
std::vector<float> curveTable(const std::vector<Vec2>& points, int n, bool periodic);
// Points sorted by x, x clamped to 0..1, at most one point per x (later wins).
std::vector<Vec2> normalizeCurve(std::vector<Vec2> points);

}  // namespace fa
