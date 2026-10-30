#include "fa/render/transitions.h"

#include <algorithm>
#include <cmath>

namespace fa {

const std::vector<TransitionInfo>& transitionTypes() {
    static const std::vector<TransitionInfo> types = {
        {"fa.transition.dissolve", "Dissolve", "Cross-dissolve: the next clip fades in over the current one.", false, false},
        {"fa.transition.dip_to_black", "Fade", "Fade through black: the clip fades to black, the next one fades in.", false, false},
        {"fa.transition.wipe", "Wipe", "A soft edge sweeps across and reveals the next clip.", true, true},
        {"fa.transition.slide", "Slide", "The next clip slides in over the current one.", true, false},
        {"fa.transition.push", "Push", "The next clip pushes the current one out of the frame.", true, false},
        {"fa.transition.zoom", "Zoom", "The current clip zooms in and fades out, revealing the next one.", false, false},
        // M11
        {"fa.transition.dip_to_white", "Dip to White", "The clip fades to white, then the next one fades in from white.",
         false, false},
        {"fa.transition.flash", "Flash", "A bright white flash hides the cut between the two clips.", false, false},
        {"fa.transition.circle_open", "Circle Open", "A circle grows from the centre and reveals the next clip.", false, true},
        {"fa.transition.circle_close", "Circle Close", "The clip shrinks into a circle and closes on the next one.", false,
         true},
        {"fa.transition.clock", "Clock Wipe", "A clock hand sweeps around and reveals the next clip.", false, true},
        {"fa.transition.barn_door", "Barn Door", "Two doors open from the middle and reveal the next clip.", true, true},
        {"fa.transition.diamond", "Diamond", "A diamond grows from the centre and reveals the next clip.", false, true},
        {"fa.transition.blinds", "Blinds", "Ten stripes open together like window blinds.", true, true},
        {"fa.transition.diagonal", "Diagonal Wipe", "A soft edge sweeps from one corner to the opposite one.", true, true},
        {"fa.transition.spin", "Spin", "Both clips turn half a turn while the next one fades in.", false, false},
        {"fa.transition.zoom_in", "Zoom In", "The next clip grows from the centre over the current one.", false, false},
        {"fa.transition.blur", "Blur", "The clip blurs out and the next one comes into focus.", false, false},
        {"fa.transition.pixelate", "Pixelate", "The clip breaks into big pixels that resolve into the next one.", false,
         false},
    };
    return types;
}

const TransitionInfo* findTransition(const std::string& id) {
    for (const auto& t : transitionTypes())
        if (id == t.id) return &t;
    return nullptr;
}

double easeInOutQuart(double p) {
    p = std::clamp(p, 0.0, 1.0);
    return p < 0.5 ? 8 * p * p * p * p : 1 - std::pow(-2 * p + 2, 4) / 2;
}

namespace {
void dirVector(TransitionDirection d, double& x, double& y) {
    x = y = 0;
    switch (d) {
        case TransitionDirection::Left: x = -1; break;
        case TransitionDirection::Right: x = 1; break;
        case TransitionDirection::Up: y = -1; break;
        case TransitionDirection::Down: y = 1; break;
    }
}

double smoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

void reveal(LayerAdjust& l, WipeShape shape, double progress, const TransitionRef& t, double dx = 1, double dy = 0) {
    l.wipe.active = true;
    l.wipe.shape = shape;
    l.wipe.dirX = dx;
    l.wipe.dirY = dy;
    l.wipe.progress = progress;
    l.wipe.softness = std::clamp(t.softness, 0.0, 1.0);
}
}  // namespace

TransitionLayers evaluateTransition(const TransitionRef& t, double p) {
    p = std::clamp(p, 0.0, 1.0);
    TransitionLayers out;
    double dx = 0, dy = 0;
    dirVector(t.direction, dx, dy);
    const std::string& id = t.type;
    if (id == "fa.transition.dip_to_black") {
        if (p < 0.5) {
            out.a.brightness = 1 - 2 * p;
            out.b.visible = false;
        } else {
            out.b.brightness = 2 * p - 1;
            out.a.visible = false;
        }
    } else if (id == "fa.transition.wipe") {
        reveal(out.b, WipeShape::Linear, p, t, dx, dy);
    } else if (id == "fa.transition.slide") {
        const double e = easeInOutQuart(p);
        out.b.dx = -dx * (1 - e);   // B starts one frame away, opposite to the direction of travel
        out.b.dy = -dy * (1 - e);
    } else if (id == "fa.transition.push") {
        const double e = easeInOutQuart(p);
        out.a.dx = dx * e;
        out.a.dy = dy * e;
        out.b.dx = -dx * (1 - e);
        out.b.dy = -dy * (1 - e);
    } else if (id == "fa.transition.zoom") {
        const double e = easeInOutQuart(p);
        out.bFirst = true;
        out.a.scale = 1 + 0.6 * e;
        out.a.opacity = 1 - e;
        out.b.scale = 1.15 - 0.15 * e;
    } else if (id == "fa.transition.dip_to_white") {
        if (p < 0.5) {
            out.a.flash = 2 * p;
            out.b.visible = false;
        } else {
            out.b.flash = 2 - 2 * p;
            out.a.visible = false;
        }
    } else if (id == "fa.transition.flash") {
        const double f = smoothstep(0.2, 0.5, p) * (1 - smoothstep(0.5, 0.8, p));
        out.a.flash = out.b.flash = f;
        if (p < 0.5) out.b.visible = false;
        else out.a.visible = false;
    } else if (id == "fa.transition.circle_open") {
        reveal(out.b, WipeShape::Circle, p, t);
    } else if (id == "fa.transition.circle_close") {
        out.bFirst = true;   // A on top, kept inside the shrinking circle
        reveal(out.a, WipeShape::Circle, 1 - p, t);
    } else if (id == "fa.transition.clock") {
        reveal(out.b, WipeShape::Clock, p, t);
    } else if (id == "fa.transition.barn_door") {
        reveal(out.b, WipeShape::Split, p, t, dx, dy);
    } else if (id == "fa.transition.diamond") {
        reveal(out.b, WipeShape::Diamond, p, t);
    } else if (id == "fa.transition.blinds") {
        reveal(out.b, WipeShape::Blinds, p, t, dx, dy);
        out.b.wipe.stripes = 10;
    } else if (id == "fa.transition.diagonal") {
        // corner to corner: right = from the top left, left = from the bottom right, up = from the bottom left,
        // down = from the top right (the edge reaches the far corner exactly at p = 1)
        const double ux = dx != 0 ? 0.5 * dx : (dy < 0 ? 0.5 : -0.5);
        const double uy = dx != 0 ? 0.5 * dx : (dy < 0 ? -0.5 : 0.5);
        reveal(out.b, WipeShape::Linear, p, t, ux, uy);
    } else if (id == "fa.transition.spin") {
        const double e = easeInOutQuart(p);
        const double s = 1 - 0.45 * std::sin(3.14159265358979323846 * e);
        out.a.rotation = 180 * e;
        out.b.rotation = 180 * e - 180;
        out.a.scale = out.b.scale = s;
        out.b.opacity = smoothstep(0.35, 0.65, p);
    } else if (id == "fa.transition.zoom_in") {
        const double e = easeInOutQuart(p);
        out.b.scale = 0.25 + 0.75 * e;
        out.b.opacity = std::min(1.0, 2.5 * e);
    } else if (id == "fa.transition.blur") {
        out.a.blur = out.b.blur = 60 * std::sin(3.14159265358979323846 * p);
        out.b.opacity = smoothstep(0.25, 0.75, p);
    } else if (id == "fa.transition.pixelate") {
        const double size = 1 + 80 * std::pow(1 - std::abs(2 * p - 1), 1.5);
        out.a.pixelate = out.b.pixelate = size;
        if (p < 0.5) out.b.visible = false;
        else out.a.visible = false;
    } else {   // dissolve (and the default for unknown ids is rejected by preflight)
        out.b.opacity = p;
    }
    return out;
}

}  // namespace fa
