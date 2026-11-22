#include "UiTheme.h"
#include <cmath>

namespace zerocam::ui {

int ScaleDpi(int px, float dpiScale) {
    return static_cast<int>(std::lround(static_cast<float>(px) * dpiScale));
}

float ScaleDpif(float px, float dpiScale) {
    return px * dpiScale;
}

DoubleBufferPaint::DoubleBufferPaint(HDC targetDc, int width, int height)
    : targetDc_(targetDc), width_(std::max(1, width)), height_(std::max(1, height)) {
    memDc_ = CreateCompatibleDC(targetDc_);
    bitmap_ = CreateCompatibleBitmap(targetDc_, width_, height_);
    if (memDc_ && bitmap_) {
        oldBitmap_ = SelectObject(memDc_, bitmap_);
    }
}

DoubleBufferPaint::~DoubleBufferPaint() {
    if (memDc_ && targetDc_) {
        BitBlt(targetDc_, 0, 0, width_, height_, memDc_, 0, 0, SRCCOPY);
    }
    if (memDc_ && oldBitmap_) {
        SelectObject(memDc_, oldBitmap_);
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
    }
    if (memDc_) {
        DeleteDC(memDc_);
    }
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rc, float radius) {
    const float r = std::min({ radius, rc.Width * 0.5f, rc.Height * 0.5f });
    if (r <= 0.5f) {
        path.AddRectangle(rc);
        return;
    }
    const float d = r * 2.0f;
    path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, float radius, Gdiplus::Color fill) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, rc, radius);
    Gdiplus::SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

void DrawRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, float radius, Gdiplus::Color stroke, float width) {
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, rc, radius);
    Gdiplus::Pen pen(stroke, width);
    g.DrawPath(&pen, &path);
}

void DrawTextUi(
    Gdiplus::Graphics& g,
    const std::wstring& text,
    const Gdiplus::RectF& bounds,
    const wchar_t* fontName,
    float fontSizePx,
    bool bold,
    Gdiplus::Color color,
    Gdiplus::StringAlignment hAlign,
    Gdiplus::StringAlignment vAlign,
    bool allowWrap) {
    Gdiplus::FontFamily family(fontName);
    Gdiplus::Font font(&family, fontSizePx, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(color);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(hAlign);
    fmt.SetLineAlignment(vAlign);
    if (!allowWrap) {
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    }
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, bounds, &fmt, &brush);
}

namespace {

void DrawEyeVector(Gdiplus::Graphics& g, float cx, float cy, float sz, bool slashed, Gdiplus::Color color, float dpiScale) {
    Gdiplus::Pen pen(color, std::max(1.25f, 1.4f * dpiScale));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    const float rx = sz * 0.44f;
    const float ry = sz * 0.28f;
    Gdiplus::GraphicsPath eyePath;
    Gdiplus::PointF topPts[3] = {
        { cx - rx, cy },
        { cx, cy - ry * 1.45f },
        { cx + rx, cy }
    };
    Gdiplus::PointF botPts[3] = {
        { cx + rx, cy },
        { cx, cy + ry * 1.45f },
        { cx - rx, cy }
    };
    eyePath.AddCurve(topPts, 3, 0.5f);
    eyePath.AddCurve(botPts, 3, 0.5f);
    g.DrawPath(&pen, &eyePath);

    const float pr = sz * 0.14f;
    g.DrawEllipse(&pen, cx - pr, cy - pr, pr * 2.0f, pr * 2.0f);

    if (slashed) {
        const float d = sz * 0.36f;
        g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
    }
}

void DrawGearVector(Gdiplus::Graphics& g, float cx, float cy, float sz, Gdiplus::Color color, float dpiScale) {
    Gdiplus::Pen pen(color, std::max(1.25f, 1.4f * dpiScale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    const float rOuter = sz * 0.34f;
    const float rInner = sz * 0.14f;
    g.DrawEllipse(&pen, cx - rOuter, cy - rOuter, rOuter * 2.0f, rOuter * 2.0f);
    g.DrawEllipse(&pen, cx - rInner, cy - rInner, rInner * 2.0f, rInner * 2.0f);

    constexpr float kPi = 3.14159265f;
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * (kPi * 0.25f);
        const float c = std::cos(a);
        const float s = std::sin(a);
        g.DrawLine(&pen, cx + c * (sz * 0.34f), cy + s * (sz * 0.34f), cx + c * (sz * 0.45f), cy + s * (sz * 0.45f));
    }
}

void DrawSlidersVector(Gdiplus::Graphics& g, float cx, float cy, float sz, Gdiplus::Color color, float dpiScale) {
    Gdiplus::Pen pen(color, std::max(1.25f, 1.4f * dpiScale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush knobBr(color);

    const float halfW = sz * 0.44f;
    const float ys[3] = { cy - sz * 0.28f, cy, cy + sz * 0.28f };
    const float kx[3] = { cx - sz * 0.16f, cx + sz * 0.18f, cx - sz * 0.06f };
    const float kr = std::max(1.8f, sz * 0.11f);
    for (int i = 0; i < 3; ++i) {
        g.DrawLine(&pen, cx - halfW, ys[i], cx + halfW, ys[i]);
        g.FillEllipse(&knobBr, kx[i] - kr, ys[i] - kr, kr * 2.0f, kr * 2.0f);
    }
}

void DrawChevronDown(Gdiplus::Graphics& g, float cx, float cy, float sz, Gdiplus::Color color, float dpiScale) {
    Gdiplus::Pen pen(color, std::max(1.2f, 1.4f * dpiScale));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float dx = sz * 0.28f;
    const float dy = sz * 0.16f;
    Gdiplus::PointF pts[3] = {
        { cx - dx, cy - dy },
        { cx, cy + dy },
        { cx + dx, cy - dy }
    };
    g.DrawLines(&pen, pts, 3);
}

} // namespace

float MeasurePillBadgeWidth(Gdiplus::Graphics& g, const std::wstring& label, float dpiScale) {
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, ScaleDpif(11.5f, dpiScale), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::RectF measured;
    g.MeasureString(label.c_str(), static_cast<INT>(label.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &measured);
    return std::ceil(measured.Width + ScaleDpif(34.0f, dpiScale));
}

void DrawPillBadge(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, Gdiplus::Color dotColor, const std::wstring& label, float dpiScale) {
    const float radius = rc.Height * 0.5f;
    FillRoundRect(g, rc, radius, kBgInput);
    DrawRoundRect(g, rc, radius, kBorderSubtle, std::max(1.0f, dpiScale));

    const float dotSz = ScaleDpif(7.5f, dpiScale);
    const float dotX = rc.X + ScaleDpif(10.0f, dpiScale);
    const float dotY = rc.Y + (rc.Height - dotSz) * 0.5f;
    Gdiplus::SolidBrush dotBrush(dotColor);
    g.FillEllipse(&dotBrush, dotX, dotY, dotSz, dotSz);

    const float textX = dotX + dotSz + ScaleDpif(6.0f, dpiScale);
    const Gdiplus::RectF textRc(textX, rc.Y, std::max(1.0f, rc.GetRight() - textX - ScaleDpif(8.0f, dpiScale)), rc.Height);
    DrawTextUi(g, label, textRc, L"Segoe UI", ScaleDpif(11.5f, dpiScale), true, kTextPrimary, Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter);
}

void DrawToggleSwitch(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, bool on, bool hover, float dpiScale) {
    const float radius = rc.Height * 0.5f;
    const Gdiplus::Color trackFill = on
        ? (hover ? kAccentHover : kAccentPrimary)
        : (hover ? kBgInputHover : kBgInput);
    FillRoundRect(g, rc, radius, trackFill);
    if (!on) {
        DrawRoundRect(g, rc, radius, hover ? kBorderHover : kBorderSubtle, std::max(1.0f, dpiScale));
    }

    const float pad = ScaleDpif(3.0f, dpiScale);
    const float knobSz = rc.Height - pad * 2.0f;
    const float knobX = on ? (rc.X + rc.Width - knobSz - pad) : (rc.X + pad);
    const float knobY = rc.Y + pad;
    const Gdiplus::Color knobCol = on ? Gdiplus::Color(255, 255, 255, 255) : kTextSecondary;
    Gdiplus::SolidBrush knobBrush(knobCol);
    g.FillEllipse(&knobBrush, knobX, knobY, knobSz, knobSz);
}

void DrawDropdownBox(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const std::wstring& value, bool hover, float dpiScale) {
    const float radius = ScaleDpif(8.0f, dpiScale);
    FillRoundRect(g, rc, radius, hover ? kBgInputHover : kBgInput);
    DrawRoundRect(g, rc, radius, hover ? kBorderHover : kBorderSubtle, std::max(1.0f, dpiScale));

    const float padL = ScaleDpif(11.0f, dpiScale);
    const float chevArea = ScaleDpif(24.0f, dpiScale);
    const Gdiplus::RectF textRc(rc.X + padL, rc.Y, std::max(1.0f, rc.Width - padL - chevArea), rc.Height);
    DrawTextUi(g, value, textRc, L"Segoe UI", ScaleDpif(12.0f, dpiScale), false, kTextPrimary, Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter);

    const float chevCx = rc.GetRight() - ScaleDpif(14.0f, dpiScale);
    const float chevCy = rc.Y + rc.Height * 0.5f;
    DrawChevronDown(g, chevCx, chevCy, ScaleDpif(12.0f, dpiScale), kTextSecondary, dpiScale);
}

void DrawButton(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& rc,
    const std::wstring& label,
    ButtonKind kind,
    IconKind icon,
    bool hover,
    bool activeBorder,
    float dpiScale) {
    const float radius = ScaleDpif(8.0f, dpiScale);
    Gdiplus::Color fill = kBgInput;
    Gdiplus::Color stroke = kBorderSubtle;
    bool drawStroke = true;

    if (kind == ButtonKind::Primary) {
        fill = hover ? kAccentHover : kAccentPrimary;
        drawStroke = activeBorder;
        stroke = Gdiplus::Color(255, 110, 170, 255);
    } else if (kind == ButtonKind::Danger) {
        fill = hover ? kStatusDangerHover : kStatusDanger;
        drawStroke = activeBorder;
        stroke = Gdiplus::Color(255, 255, 130, 130);
    } else {
        fill = hover ? kBgInputHover : kBgInput;
        stroke = activeBorder ? kBorderAccent : (hover ? kBorderHover : kBorderSubtle);
    }

    FillRoundRect(g, rc, radius, fill);
    if (drawStroke) {
        DrawRoundRect(g, rc, radius, stroke, activeBorder ? ScaleDpif(1.5f, dpiScale) : std::max(1.0f, dpiScale));
    }

    const Gdiplus::Color fg = (kind == ButtonKind::Secondary) ? kTextPrimary : Gdiplus::Color(255, 255, 255, 255);
    const float fontSize = ScaleDpif(12.5f, dpiScale);
    const bool bold = (kind != ButtonKind::Secondary || activeBorder);

    if (icon == IconKind::None) {
        DrawTextUi(g, label, rc, L"Segoe UI", fontSize, bold, fg, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
        return;
    }

    // Mide el texto para centrar bloque [icono + texto] dentro del botón.
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, fontSize, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::RectF measured;
    g.MeasureString(label.c_str(), static_cast<INT>(label.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &measured);

    const float iconSz = ScaleDpif(15.0f, dpiScale);
    const float gap = ScaleDpif(6.0f, dpiScale);
    const float totalW = iconSz + gap + measured.Width;
    const float startX = rc.X + std::max(ScaleDpif(8.0f, dpiScale), (rc.Width - totalW) * 0.5f);
    const float iconCx = startX + iconSz * 0.5f;
    const float iconCy = rc.Y + rc.Height * 0.5f;

    if (icon == IconKind::Eye || icon == IconKind::EyeOff) {
        DrawEyeVector(g, iconCx, iconCy, iconSz, icon == IconKind::EyeOff, kTextSecondary, dpiScale);
    } else if (icon == IconKind::Gear) {
        DrawGearVector(g, iconCx, iconCy, iconSz, activeBorder ? kAccentPrimary : kTextSecondary, dpiScale);
    } else if (icon == IconKind::Sliders) {
        DrawSlidersVector(g, iconCx, iconCy, iconSz, activeBorder ? kAccentPrimary : kTextSecondary, dpiScale);
    }

    const Gdiplus::RectF textRc(startX + iconSz + gap - ScaleDpif(2.0f, dpiScale), rc.Y,
                                std::max(1.0f, rc.GetRight() - (startX + iconSz + gap) - ScaleDpif(4.0f, dpiScale)), rc.Height);
    DrawTextUi(g, label, textRc, L"Segoe UI", fontSize, bold, fg, Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter);
}

void DrawBoltIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& bounds, Gdiplus::Color color) {
    const float x = bounds.X;
    const float y = bounds.Y;
    const float w = bounds.Width;
    const float h = bounds.Height;
    Gdiplus::Pen pen(color, std::max(2.0f, w * 0.075f));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    Gdiplus::PointF pts[6] = {
        { x + w * 0.56f, y + h * 0.12f },
        { x + w * 0.24f, y + h * 0.55f },
        { x + w * 0.50f, y + h * 0.55f },
        { x + w * 0.44f, y + h * 0.88f },
        { x + w * 0.76f, y + h * 0.45f },
        { x + w * 0.50f, y + h * 0.45f }
    };
    g.DrawPolygon(&pen, pts, 6);
}

void DrawCloseIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& bounds, Gdiplus::Color color, bool hover, float dpiScale) {
    if (hover) {
        FillRoundRect(g, bounds, ScaleDpif(6.0f, dpiScale), kBgInput);
    }
    const float cx = bounds.X + bounds.Width * 0.5f;
    const float cy = bounds.Y + bounds.Height * 0.5f;
    const float r = std::min(bounds.Width, bounds.Height) * 0.24f;
    Gdiplus::Pen pen(hover ? kTextPrimary : color, std::max(1.4f, 1.5f * dpiScale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r);
    g.DrawLine(&pen, cx + r, cy - r, cx - r, cy + r);
}

void DrawViewfinderPlaceholder(Gdiplus::Graphics& g, int width, int height, const std::wstring& subtitle, float dpiScale) {
    Gdiplus::SolidBrush bgBrush(kBgPreview);
    g.FillRectangle(&bgBrush, 0, 0, width, height);

    const float pad = ScaleDpif(34.0f, dpiScale);
    const float arm = ScaleDpif(28.0f, dpiScale);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    if (w > pad * 3.0f && h > pad * 3.0f) {
        Gdiplus::Pen cornerPen(Gdiplus::Color(185, 45, 125, 255), ScaleDpif(2.0f, dpiScale));
        // Superior izquierda
        g.DrawLine(&cornerPen, pad, pad + arm, pad, pad);
        g.DrawLine(&cornerPen, pad, pad, pad + arm, pad);
        // Superior derecha
        g.DrawLine(&cornerPen, w - pad - arm, pad, w - pad, pad);
        g.DrawLine(&cornerPen, w - pad, pad, w - pad, pad + arm);
        // Inferior izquierda
        g.DrawLine(&cornerPen, pad, h - pad - arm, pad, h - pad);
        g.DrawLine(&cornerPen, pad, h - pad, pad + arm, h - pad);
        // Inferior derecha
        g.DrawLine(&cornerPen, w - pad - arm, h - pad, w - pad, h - pad);
        g.DrawLine(&cornerPen, w - pad, h - pad - arm, w - pad, h - pad);
    }

    // Silueta central sutil tipo encuadre de cámara (igual que en Figma 10:117)
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float ringR = std::min(w, h) * 0.26f;
    if (ringR > 24.0f) {
        Gdiplus::Pen dashPen(kBorderSubtle, std::max(1.0f, dpiScale));
        dashPen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawEllipse(&dashPen, cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f);

        const float headR = ringR * 0.34f;
        Gdiplus::SolidBrush silBrush(Gdiplus::Color(255, 22, 30, 48));
        Gdiplus::Pen silPen(Gdiplus::Color(220, 45, 92, 175), ScaleDpif(1.5f, dpiScale));
        const Gdiplus::RectF headRc(cx - headR, cy - ringR * 0.56f, headR * 2.0f, headR * 2.0f);
        g.FillEllipse(&silBrush, headRc);
        g.DrawEllipse(&silPen, headRc);

        const float shW = ringR * 1.16f;
        const float shH = ringR * 0.72f;
        const Gdiplus::RectF shRc(cx - shW * 0.5f, headRc.GetBottom() + ScaleDpif(6.0f, dpiScale), shW, shH);
        g.FillPie(&silBrush, shRc.X, shRc.Y, shRc.Width, shRc.Height * 1.8f, 180.0f, 180.0f);
        g.DrawArc(&silPen, shRc.X, shRc.Y, shRc.Width, shRc.Height * 1.8f, 180.0f, 180.0f);
    }

    // Cápsula superior izquierda ("LIVE / Esperando señal...")
    const float badgeW = MeasurePillBadgeWidth(g, subtitle, dpiScale);
    const Gdiplus::RectF pillRc(ScaleDpif(14.0f, dpiScale), ScaleDpif(14.0f, dpiScale), badgeW, ScaleDpif(26.0f, dpiScale));
    DrawPillBadge(g, pillRc, kStatusWarning, subtitle, dpiScale);
}

void DrawSliderControl(Gdiplus::Graphics& g, const Gdiplus::RectF& trackRc, float norm01, bool hover, bool active, float dpiScale) {
    const float clamped = std::clamp(norm01, 0.0f, 1.0f);
    const float trackH = ScaleDpif(5.0f, dpiScale);
    const float trackY = trackRc.Y + (trackRc.Height - trackH) * 0.5f;
    const Gdiplus::RectF barRc(trackRc.X, trackY, trackRc.Width, trackH);
    const float rad = trackH * 0.5f;

    FillRoundRect(g, barRc, rad, kBgInput);
    DrawRoundRect(g, barRc, rad, hover ? kBorderHover : kBorderSubtle, std::max(1.0f, dpiScale * 0.8f));

    const float fillW = std::max(trackH, trackRc.Width * clamped);
    const Gdiplus::RectF fillRc(trackRc.X, trackY, std::min(trackRc.Width, fillW), trackH);
    FillRoundRect(g, fillRc, rad, (hover || active) ? kAccentHover : kAccentPrimary);

    const float knobR = ScaleDpif(active ? 7.5f : (hover ? 7.0f : 6.2f), dpiScale);
    const float knobCx = trackRc.X + trackRc.Width * clamped;
    const float knobCy = trackRc.Y + trackRc.Height * 0.5f;
    Gdiplus::SolidBrush knobBr(kTextPrimary);
    Gdiplus::Pen knobPen((hover || active) ? kAccentHover : kAccentPrimary, ScaleDpif(1.8f, dpiScale));
    g.FillEllipse(&knobBr, knobCx - knobR, knobCy - knobR, knobR * 2.0f, knobR * 2.0f);
    g.DrawEllipse(&knobPen, knobCx - knobR, knobCy - knobR, knobR * 2.0f, knobR * 2.0f);
}

} // namespace zerocam::ui
