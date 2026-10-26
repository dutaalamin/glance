#include "scene.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

// Strictly neutral: black panel, grey tiles, white accent. No hue anywhere, so
// the icons carry all of the colour.
const D2D1_COLOR_F kNotchBg = D2D1::ColorF(0x08080A);
const D2D1_COLOR_F kPanelBgTop = D2D1::ColorF(0x0C0C0E);
const D2D1_COLOR_F kPanelBgBottom = D2D1::ColorF(0x141416);
const D2D1_COLOR_F kTile = D2D1::ColorF(0x1A1A1D);
const D2D1_COLOR_F kTileHover = D2D1::ColorF(0x262629);
const D2D1_COLOR_F kStroke = D2D1::ColorF(0x27272B);
const D2D1_COLOR_F kStrokeSoft = D2D1::ColorF(0x1E1E21);
const D2D1_COLOR_F kTileTop = D2D1::ColorF(0x232327);
const D2D1_COLOR_F kTileBottom = D2D1::ColorF(0x151518);
const D2D1_COLOR_F kTileTopHover = D2D1::ColorF(0x2E2E33);
const D2D1_COLOR_F kTileBottomHover = D2D1::ColorF(0x1F1F23);
const D2D1_COLOR_F kText = D2D1::ColorF(0xF7F7F8);
const D2D1_COLOR_F kMuted = D2D1::ColorF(0x8B8B92);
const D2D1_COLOR_F kMint = D2D1::ColorF(0xEDEDEF);

constexpr float kGridTileH = 92.0f;  // tile height; the gradient span depends on it

// Vertical two-stop gradient used as the tile surface. Built per paint because
// gradient brushes are render-target resources, not factory ones.
ID2D1LinearGradientBrush* makeTileGradient(ID2D1RenderTarget* rt, const D2D1_COLOR_F& top,
                                          const D2D1_COLOR_F& bottom) {
  D2D1_GRADIENT_STOP stops[2]{};
  stops[0].position = 0.0f;
  stops[0].color = top;
  stops[1].position = 1.0f;
  stops[1].color = bottom;

  ID2D1GradientStopCollection* collection = nullptr;
  if (FAILED(rt->CreateGradientStopCollection(stops, 2, &collection)) || !collection) {
    return nullptr;
  }
  ID2D1LinearGradientBrush* brush = nullptr;
  rt->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                          D2D1::Point2F(0.0f, kGridTileH)),
      collection, &brush);
  collection->Release();
  return brush;
}

void fillRound(ID2D1RenderTarget* rt, ID2D1Brush* b, const D2D1_RECT_F& r, float rad) {
  rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b);
}

void strokeRound(ID2D1RenderTarget* rt, ID2D1Brush* b, const D2D1_RECT_F& r,
                 float rad) {
  rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b, 1.0f);
}

void textAt(ID2D1RenderTarget* rt, IDWriteTextFormat* fmt, const wchar_t* s, const D2D1_RECT_F& r,
            ID2D1SolidColorBrush* b, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
  fmt->SetTextAlignment(align);
  rt->DrawTextW(s, static_cast<UINT32>(wcslen(s)), fmt, r, b);
}


std::wstring elide(const std::wstring& s, size_t maxChars) {
  if (s.size() <= maxChars) return s;
  return s.substr(0, maxChars - 1) + L"\u2026";
}


}  // namespace

namespace {
constexpr float kGridGap = 12.0f;
constexpr float kPanelTop = 38.0f;   // notch strip height
constexpr float kGridTop = 18.0f;    // space under the notch
constexpr float kFooterH = 16.0f;  // bottom breathing room only; no caption row
constexpr float kIdleH = 68.0f;
constexpr float kMediaH = 148.0f;
constexpr float kSeekHitH = 22.0f;
// Must match drawVolumeBlock's glyph + gap + slider + gap + percentage maths.
constexpr float kVolumeBlockW = 14.0f + 8.0f + 64.0f;
constexpr float kCardPad = 16.0f;
}  // namespace

float Scene::panelHeightFor(size_t appCount, bool withMedia) {
  const int cols = appCount <= 4 ? static_cast<int>(appCount) : 4;
  const int count = static_cast<int>(appCount);
  const int rows = cols > 0 ? (count + cols - 1) / cols : 0;

  const float grid = rows > 0 ? static_cast<float>(rows) * kGridTileH +
                                     static_cast<float>(rows - 1) * kGridGap
                               : 0.0f;
  // The strip above the grid is the media card when something is playing and a
  // bare volume slider otherwise, so the grid never jumps between the two.
  const float strip = withMedia ? kMediaH : kIdleH;
  return kPanelTop + kGridTop + strip + grid + kFooterH;
}

bool Scene::init(IDWriteFactory* dwrite, ID2D1Factory* d2d, Launcher* launcher) {
  d2d_ = d2d;
  dwrite_ = dwrite;
  launcher_ = launcher;

  auto make = [&](IDWriteTextFormat** out, DWRITE_FONT_WEIGHT weight, float size) {
    if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           size, L"en-us", out))) {
      return true;
    }
    return SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                              DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out));
  };

  if (!make(&time_, DWRITE_FONT_WEIGHT_BOLD, 40.0f)) return false;
  if (!make(&timeNotch_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f)) return false;
  if (!make(&date_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f)) return false;
  if (!make(&label_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.5f)) return false;
  if (!make(&value_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 24.0f)) return false;
  if (!make(&small_, DWRITE_FONT_WEIGHT_NORMAL, 11.0f)) return false;
  if (!make(&row_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f)) return false;
  if (!make(&rowRight_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.5f)) return false;
  if (!make(&tile_, DWRITE_FONT_WEIGHT_NORMAL, 10.0f)) return false;
  tile_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  tile_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  // Fixed line height keeps a two-line label inside its reserved band instead of
  // growing upward into the icon.
  tile_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 12.0f, 11.0f);
  if (!make(&tiny_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.0f)) return false;

  return true;
}

void Scene::release() {
  auto drop = [](IDWriteTextFormat*& p) {
    if (p) {
      p->Release();
      p = nullptr;
    }
  };
  drop(time_);
  drop(timeNotch_);
  drop(date_);
  drop(label_);
  drop(value_);
  drop(small_);
  drop(row_);
  drop(rowRight_);
  drop(tile_);
  drop(tiny_);

  discardArtBitmap();
}

void Scene::draw(ID2D1RenderTarget* rt, const Snapshot& s, PanelMode mode, float w, float h) {
  const bool expanded = mode == PanelMode::Expanded;

  rt->Clear(kNotchBg);

  if (!expanded) {
    ID2D1SolidColorBrush* bg = nullptr;
    rt->CreateSolidColorBrush(kNotchBg, &bg);
    fillRound(rt, bg, D2D1::RectF(0.0f, 0.0f, w, h), h * 0.5f);

    ID2D1SolidColorBrush* hair = nullptr;
    rt->CreateSolidColorBrush(kStrokeSoft, &hair);
    strokeRound(rt, hair, D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), h * 0.5f);

    if (hair) hair->Release();
    if (bg) bg->Release();
  } else {
    // Vertical gradient gives the panel a little depth without a texture.
    ID2D1GradientStopCollection* stops = nullptr;
    const D2D1_GRADIENT_STOP gs[2] = {{0.0f, kPanelBgTop}, {1.0f, kPanelBgBottom}};
    if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 2, &stops)) && stops) {
      ID2D1LinearGradientBrush* grad = nullptr;
      if (SUCCEEDED(rt->CreateLinearGradientBrush(
              D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                  D2D1::Point2F(0.0f, h)),
              stops, &grad)) &&
          grad) {
        fillRound(rt, grad, D2D1::RectF(0.0f, 0.0f, w, h), 26.0f);
        grad->Release();
      }
      stops->Release();
    }

    ID2D1SolidColorBrush* hair = nullptr;
    rt->CreateSolidColorBrush(kStroke, &hair);
    strokeRound(rt, hair, D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 26.0f);
    if (hair) hair->Release();
  }

  drawNotch(rt, s, w, expanded ? 38.0f : h, expanded);
  if (expanded) drawPanel(rt, s, w, h, 38.0f);
}

float Scene::measureText(const wchar_t* text, IDWriteTextFormat* format) {
  if (!dwrite_ || !format || !text) return 0.0f;

  IDWriteTextLayout* layout = nullptr;
  if (FAILED(dwrite_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format,
                                       1000.0f, 100.0f, &layout)) ||
      !layout) {
    return 0.0f;
  }

  DWRITE_TEXT_METRICS metrics{};
  const float width = SUCCEEDED(layout->GetMetrics(&metrics)) ? metrics.width : 0.0f;
  layout->Release();
  return width;
}

void Scene::drawNotch(ID2D1RenderTarget* rt, const Snapshot& s, float w, float h,
                      bool expanded) {
  (void)expanded;

  ID2D1SolidColorBrush *textB = nullptr, *mutedB = nullptr, *accentB = nullptr;
  rt->CreateSolidColorBrush(kText, &textB);
  rt->CreateSolidColorBrush(kMuted, &mutedB);
  rt->CreateSolidColorBrush(kMint, &accentB);

  const float cy = h * 0.5f;

  // While something plays, the track takes the centre of the notch and the
  // clock steps aside - the same trade the Windows media overlay makes.
  if (media_.valid() && media_.playing) {
    const std::wstring line = L"\u266A  " + elide(media_.title, 18) +
                              (media_.artist.empty() ? L"" : L"  \u00B7  " + elide(media_.artist, 16));
    textAt(rt, timeNotch_, line.c_str(), D2D1::RectF(0.0f, cy - 10.0f, w, cy + 10.0f),
           textB, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (mutedB) mutedB->Release();
    if (textB) textB->Release();
    if (accentB) accentB->Release();
    return;
  }

  wchar_t clock[16]{};
  swprintf(clock, 16, L"%02d:%02d", s.hour, s.minute);

  // Notification icons sit beside the clock, not in the panel. The notch is the
  // only thing visible while collapsed, so that is where a glance has to land.
  const int maxIcons = ambient_ ? std::min<int>(static_cast<int>(ambient_->badges.size()), 4) : 0;

  if (maxIcons <= 0) {
    textAt(rt, timeNotch_, clock, D2D1::RectF(0.0f, cy - 10.0f, w, cy + 10.0f), textB,
           DWRITE_TEXT_ALIGNMENT_CENTER);

    if (mutedB) mutedB->Release();
    if (textB) textB->Release();
    if (accentB) accentB->Release();
    return;
  }

  const float iconSize = 15.0f;
  const float iconGap = 7.0f;
  const float clockGap = 8.0f;

  const float clockW = measureText(clock, timeNotch_);
  const float iconsW = static_cast<float>(maxIcons) * iconSize +
                       static_cast<float>(maxIcons - 1) * iconGap;
  const float groupW = clockW + (clockW > 0.0f ? clockGap : 0.0f) + iconsW;

  // Centre the clock-plus-icons group as one unit, so the notch still reads as
  // balanced rather than the clock drifting left as badges appear.
  float x = (w - groupW) * 0.5f;
  if (x < 4.0f) x = 4.0f;

  textAt(rt, timeNotch_, clock, D2D1::RectF(x, cy - 10.0f, x + clockW + 2.0f, cy + 10.0f),
         textB, DWRITE_TEXT_ALIGNMENT_LEADING);

  float ix = x + clockW + (clockW > 0.0f ? clockGap : 0.0f);
  for (int i = 0; i < maxIcons; ++i) {
    const AppBadge& badge = ambient_->badges[static_cast<size_t>(i)];
    const D2D1_RECT_F r =
        D2D1::RectF(ix, cy - iconSize * 0.5f, ix + iconSize, cy + iconSize * 0.5f);

    if (badge.icon) {
      rt->DrawBitmap(badge.icon, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      // No icon resolved: a dot still says "something happened here".
      rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + iconSize * 0.5f, cy), 3.5f, 3.5f),
                      accentB);
    }

    ix += iconSize + iconGap;
  }

  if (accentB) accentB->Release();
  if (mutedB) mutedB->Release();
  if (textB) textB->Release();
}

void Scene::drawLauncherGrid(ID2D1RenderTarget* rt, float x, float y, float w) {
  tileCount_ = 0;
  if (!launcher_ || launcher_->apps().empty()) return;

  launcher_->syncIcons(rt);

  ID2D1SolidColorBrush *tileB = nullptr, *hoverB = nullptr, *strokeB = nullptr,
                       *accentB = nullptr, *mutedB = nullptr, *textB = nullptr;

  rt->CreateSolidColorBrush(kTile, &tileB);
  rt->CreateSolidColorBrush(kTileHover, &hoverB);
  rt->CreateSolidColorBrush(kStrokeSoft, &strokeB);
  rt->CreateSolidColorBrush(kMint, &accentB);
  rt->CreateSolidColorBrush(kMuted, &mutedB);
  rt->CreateSolidColorBrush(kText, &textB);

  const auto& apps = launcher_->apps();

  ID2D1LinearGradientBrush* tileGrad = makeTileGradient(rt, kTileTop, kTileBottom);
  ID2D1LinearGradientBrush* hoverGrad = makeTileGradient(rt, kTileTopHover, kTileBottomHover);

  const float gap = 10.0f;
  const int cols = apps.size() <= 4 ? static_cast<int>(apps.size()) : 4;
  if (cols <= 0) return;

  const float tileW = (w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);

  for (size_t i = 0; i < apps.size(); ++i) {
    const int col = static_cast<int>(i) % cols;
    const int row = static_cast<int>(i) / cols;

    const float tx = x + static_cast<float>(col) * (tileW + gap);
    const float ty = y + static_cast<float>(row) * (kGridTileH + gap);
    const D2D1_RECT_F r = D2D1::RectF(tx, ty, tx + tileW, ty + kGridTileH);

    const bool hovered = (static_cast<int>(i) == hoveredTile_);

    ID2D1LinearGradientBrush* fill = hovered ? hoverGrad : tileGrad;
    if (!fill) fill = tileGrad;
    fill->SetStartPoint(D2D1::Point2F(r.left, r.top));
    fill->SetEndPoint(D2D1::Point2F(r.left, r.bottom));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 20.0f, 20.0f), fill);
    strokeRound(rt, hovered ? accentB : strokeB, r, 20.0f);

    const float iconSize = 38.0f;
    const float iconTop = ty + 14.0f;
    const D2D1_RECT_F iconRect = D2D1::RectF(tx + tileW * 0.5f - iconSize * 0.5f, iconTop,
                                             tx + tileW * 0.5f + iconSize * 0.5f,
                                             iconTop + iconSize);

    if (apps[i].icon) {
      rt->DrawBitmap(apps[i].icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      rt->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(tx + tileW * 0.5f, iconTop + iconSize * 0.5f), 10.0f, 10.0f),
          accentB);
    }

    // Two lines maximum. Names like "Chrome Remote Desktop" read badly cut to
    // one, so wrap on words and reserve the height up front.
    const std::wstring label = elide(apps[i].name, 24);
    textAt(rt, tile_, label.c_str(),
           D2D1::RectF(r.left + 6.0f, ty + kGridTileH - 38.0f, r.right - 6.0f,
                       ty + kGridTileH - 9.0f),
           hovered ? textB : mutedB, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (tileCount_ < 12) tileRects_[tileCount_++] = r;
  }

  if (hoverGrad) hoverGrad->Release();
  if (tileGrad) tileGrad->Release();
  if (textB) textB->Release();
  if (mutedB) mutedB->Release();
  if (accentB) accentB->Release();
  if (strokeB) strokeB->Release();
  if (hoverB) hoverB->Release();
  if (tileB) tileB->Release();
}

namespace {

// Battery: outline, proportional fill, terminal nub, and a bolt while charging.
// Drawn as a real glyph rather than a bar so it is recognisable at 11 px tall.
void drawBattery(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* stroke,
                 ID2D1SolidColorBrush* fill, ID2D1SolidColorBrush* cut, float right,
                 float cy, int pct, bool charging) {
  const float w = 24.0f;
  const float h = 11.0f;
  const float left = right - w;
  const D2D1_RECT_F body = D2D1::RectF(left, cy - h * 0.5f, left + w, cy + h * 0.5f);

  rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 3.0f, 3.0f), stroke, 1.2f);
  rt->FillRectangle(D2D1::RectF(body.right + 1.0f, cy - 2.0f, body.right + 3.0f, cy + 2.0f),
                    stroke);

  if (pct >= 0) {
    const float innerW = (w - 4.0f) * (static_cast<float>(pct) / 100.0f);
    if (innerW > 0.5f) {
      rt->FillRectangle(
          D2D1::RectF(body.left + 2.0f, body.top + 2.0f, body.left + 2.0f + innerW,
                      body.bottom - 2.0f),
          fill);
    }
  }

  if (charging && factory) {
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && geo && SUCCEEDED(geo->Open(&sink))) {
      const float cx = left + w * 0.5f;
      sink->BeginFigure(D2D1::Point2F(cx + 2.5f, cy - 5.0f), D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(D2D1::Point2F(cx - 2.5f, cy + 0.5f));
      sink->AddLine(D2D1::Point2F(cx - 0.2f, cy + 0.5f));
      sink->AddLine(D2D1::Point2F(cx - 2.0f, cy + 5.0f));
      sink->AddLine(D2D1::Point2F(cx + 3.0f, cy - 1.0f));
      sink->AddLine(D2D1::Point2F(cx + 0.5f, cy - 1.0f));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      // Cut-out only reads when there is fill behind it; on a nearly empty
      // battery the bolt has to be the accent colour instead.
      rt->FillGeometry(geo, pct >= 25 ? cut : stroke);
    }
    if (sink) sink->Release();
    if (geo) geo->Release();
  }
}

}  // namespace

namespace {

// Speaker glyph plus slider plus percentage, right-aligned and ending at `right`.
// Shared by the media card and the bare volume strip so both read identically.
float drawVolumeBlock(ID2D1RenderTarget* rt, ID2D1Factory* factory, IDWriteTextFormat* tiny,
                      float right, float y, float volume, bool muted, bool available,
                      D2D1_RECT_F* trackOut) {
  const float sliderW = 64.0f;
  const float glyphW = 14.0f;
  const float gap = 8.0f;
  const float blockW = glyphW + gap + sliderW;

  ID2D1SolidColorBrush* trackB = nullptr;
  ID2D1SolidColorBrush* fillB = nullptr;
  ID2D1SolidColorBrush* textB = nullptr;
  ID2D1SolidColorBrush* mutedB = nullptr;
  rt->CreateSolidColorBrush(kStrokeSoft, &trackB);
  rt->CreateSolidColorBrush(kMint, &fillB);
  rt->CreateSolidColorBrush(kText, &textB);
  rt->CreateSolidColorBrush(kMuted, &mutedB);

  const float left = right - blockW;
  const float glyphCx = left + glyphW * 0.5f;
  const float cy = y + 2.0f;

  // Speaker glyph: a small cone plus two arcs. Muted gets a slash instead.
  ID2D1PathGeometry* cone = nullptr;
  ID2D1GeometrySink* sink = nullptr;
  if (factory && SUCCEEDED(factory->CreatePathGeometry(&cone)) && cone &&
      SUCCEEDED(cone->Open(&sink))) {
    sink->BeginFigure(D2D1::Point2F(glyphCx - 6.0f, cy - 2.5f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(glyphCx - 3.0f, cy - 2.5f));
    sink->AddLine(D2D1::Point2F(glyphCx + 1.0f, cy - 6.0f));
    sink->AddLine(D2D1::Point2F(glyphCx + 1.0f, cy + 6.0f));
    sink->AddLine(D2D1::Point2F(glyphCx - 3.0f, cy + 2.5f));
    sink->AddLine(D2D1::Point2F(glyphCx - 6.0f, cy + 2.5f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->FillGeometry(cone, muted ? mutedB : textB);
  }
  if (sink) sink->Release();
  if (cone) cone->Release();

  if (muted) {
    rt->DrawLine(D2D1::Point2F(glyphCx + 3.0f, cy - 5.0f),
                 D2D1::Point2F(glyphCx + 9.0f, cy + 5.0f), mutedB, 1.4f);
  }

  const float barX = left + glyphW + gap;
  const float barY = cy - 2.0f;
  // Padding above and below so the 4 px track is still clickable.
  if (trackOut) {
    *trackOut = D2D1::RectF(barX, barY - 9.0f, barX + sliderW, barY + 13.0f);
  }
  fillRound(rt, trackB, D2D1::RectF(barX, barY, barX + sliderW, barY + 4.0f), 2.0f);

  const float effective = muted ? 0.0f : std::clamp(volume, 0.0f, 1.0f);
  if (available && effective > 0.005f) {
    fillRound(rt, fillB,
              D2D1::RectF(barX, barY, barX + std::max(sliderW * effective, 4.0f), barY + 4.0f),
              2.0f);
  }
  if (available) {
    const float knobX = barX + std::clamp(sliderW * effective, 3.0f, sliderW - 3.0f);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, barY + 2.0f), 5.0f, 5.0f), textB);
  }

  // No percentage label: the bar already shows the level, and the number was
  // competing with the slider for the same 60 dip of space.
  (void)tiny;

  if (mutedB) mutedB->Release();
  if (textB) textB->Release();
  if (fillB) fillB->Release();
  if (trackB) trackB->Release();

  return blockW;
}

}  // namespace

void Scene::discardArtBitmap() {
  if (artBitmap_) {
    artBitmap_->Release();
    artBitmap_ = nullptr;
  }
  artTarget_ = nullptr;
  artBuiltVersion_ = 0;
}

ID2D1Bitmap* Scene::artBitmap(ID2D1RenderTarget* rt) {
  if (media_.art.empty() || media_.artW <= 0 || media_.artH <= 0) return nullptr;
  if (artBitmap_ && artBuiltVersion_ == media_.artVersion && artTarget_ == rt) {
    return artBitmap_;
  }

  if (artBitmap_) {
    artBitmap_->Release();
    artBitmap_ = nullptr;
  }

  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(media_.artW),
                                          static_cast<UINT32>(media_.artH)),
                              media_.art.data(),
                              static_cast<UINT32>(media_.artW) * 4u, props, &artBitmap_)) ||
      !artBitmap_) {
    artBitmap_ = nullptr;
    return nullptr;
  }

  artBuiltVersion_ = media_.artVersion;
  artTarget_ = rt;
  return artBitmap_;
}

// The strip above the grid: now-playing when a player is active, otherwise the
// clock. Returns the height used, which the caller adds to the grid offset.
float Scene::drawMediaCard(ID2D1RenderTarget* rt, float x, float y, float w,
                           const Snapshot& snap) {
  mediaButtonsValid_ = false;

  ID2D1SolidColorBrush *tileB = nullptr, *textB = nullptr, *mutedB = nullptr,
                       *trackB = nullptr, *accentB = nullptr;
  rt->CreateSolidColorBrush(kTile, &tileB);
  rt->CreateSolidColorBrush(kText, &textB);
  rt->CreateSolidColorBrush(kMuted, &mutedB);
  rt->CreateSolidColorBrush(kStrokeSoft, &trackB);
  rt->CreateSolidColorBrush(kMint, &accentB);

  if (!media_.hasSession) {
    // Idle: date on the left; battery then volume on the right. Notification
    // icons live in the notch beside the clock instead of here, because the
    // notch is the only thing visible while the panel is collapsed.
    if (!snap.date.empty()) {
      textAt(rt, small_, snap.date.c_str(),
             D2D1::RectF(x + 4.0f, y + 22.0f, x + w * 0.52f, y + 44.0f), textB);
    }

    const float cy = y + 30.0f;
    const float volumeRight = x + w - kCardPad;
    float cursor = volumeRight - kVolumeBlockW - 14.0f;

    // Battery, right to left: glyph then percentage.
    if (ambient_ && ambient_->hasBattery) {
      drawBattery(d2d_, rt, trackB, accentB, tileB, cursor, cy, ambient_->batteryPct,
                  ambient_->charging);

      wchar_t pct[8]{};
      if (ambient_->batteryPct >= 0) {
        swprintf(pct, 8, L"%d%%", ambient_->batteryPct);
      } else {
        wcscpy_s(pct, L"--");
      }
      textAt(rt, tiny_, pct, D2D1::RectF(cursor - 56.0f, cy - 8.0f, cursor - 28.0f, cy + 8.0f),
             mutedB, DWRITE_TEXT_ALIGNMENT_TRAILING);
      cursor -= 56.0f + 12.0f;
    }

    drawVolumeBlock(rt, d2d_, tiny_, volumeRight, cy, volume_, muted_, volumeAvailable_,
                    &volumeRect_);

    if (accentB) accentB->Release();
    if (trackB) trackB->Release();
    if (mutedB) mutedB->Release();
    if (textB) textB->Release();
    if (tileB) tileB->Release();
    return kIdleH;
  }

  const D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + kMediaH - 8.0f);
  fillRound(rt, tileB, card, 18.0f);
  strokeRound(rt, trackB, card, 18.0f);

  const float innerLeft = x + kCardPad;
  const float innerWidth = w - kCardPad * 2.0f;

  // Cover art, 40 x 40, round corners via a clamped bitmap brush so no clip
  // layer is needed. Falls back to a note glyph when the player offers none.
  const float artSize = 40.0f;
  const D2D1_RECT_F artRect =
      D2D1::RectF(innerLeft, y + 12.0f, innerLeft + artSize, y + 12.0f + artSize);

  ID2D1Bitmap* art = artBitmap(rt);
  if (art) {
    ID2D1BitmapBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateBitmapBrush(art, &brush)) && brush) {
      brush->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP);
      brush->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
      const float scale = artSize / static_cast<float>(media_.artW);
      brush->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                          D2D1::Matrix3x2F::Translation(artRect.left, artRect.top));
      rt->FillRoundedRectangle(D2D1::RoundedRect(artRect, 11.0f, 11.0f), brush);
      brush->Release();
    }
  } else {
    fillRound(rt, trackB, artRect, 11.0f);
    ID2D1PathGeometry* note = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    const float cx = artRect.left + artSize * 0.5f;
    const float cy = artRect.top + artSize * 0.5f;
    if (d2d_ && SUCCEEDED(d2d_->CreatePathGeometry(&note)) && note &&
        SUCCEEDED(note->Open(&sink))) {
      sink->BeginFigure(D2D1::Point2F(cx - 1.0f, cy + 6.0f), D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(D2D1::Point2F(cx - 1.0f, cy - 6.0f));
      sink->AddLine(D2D1::Point2F(cx + 6.0f, cy - 8.0f));
      sink->AddLine(D2D1::Point2F(cx + 6.0f, cy + 2.0f));
      sink->AddLine(D2D1::Point2F(cx + 2.0f, cy + 4.0f));
      sink->AddLine(D2D1::Point2F(cx + 2.0f, cy - 3.0f));
      sink->AddLine(D2D1::Point2F(cx + 3.0f, cy - 3.5f));
      sink->AddLine(D2D1::Point2F(cx + 3.0f, cy + 6.0f));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      rt->FillGeometry(note, mutedB);
    }
    if (sink) sink->Release();
    if (note) note->Release();
  }

  const float metaLeft = artRect.right + 12.0f;
  const float metaWidth = innerLeft + innerWidth - metaLeft;

  const std::wstring title = elide(media_.title.empty() ? L"Unknown track" : media_.title, 32);
  textAt(rt, row_, title.c_str(),
         D2D1::RectF(metaLeft, y + 15.0f, metaLeft + metaWidth, y + 33.0f), textB);

  const std::wstring subtitle = elide(media_.artist.empty() ? media_.album : media_.artist, 34);
  if (!subtitle.empty()) {
    textAt(rt, tiny_, subtitle.c_str(),
           D2D1::RectF(metaLeft, y + 33.0f, metaLeft + metaWidth, y + 49.0f), mutedB);
  }

  // Seek bar. The visible track is 5 px; progressRect_ is padded vertically so
  // grabbing it does not require pixel precision.
  const float barY = y + 62.0f;
  const float barH = 5.0f;
  progressRect_ = D2D1::RectF(innerLeft, barY - (kSeekHitH - barH) * 0.5f,
                              innerLeft + innerWidth,
                              barY + barH + (kSeekHitH - barH) * 0.5f);

  fillRound(rt, trackB, D2D1::RectF(innerLeft, barY, innerLeft + innerWidth, barY + barH), 2.5f);

  const double pos = media_.currentPositionSec();
  const double dur = media_.durationSec;
  const bool seekable = dur > 0.5;

  if (seekable) {
    const float frac = static_cast<float>(std::clamp(pos / dur, 0.0, 1.0));
    if (frac > 0.002f) {
      fillRound(rt, accentB,
                D2D1::RectF(innerLeft, barY, innerLeft + std::max(innerWidth * frac, 5.0f),
                            barY + barH),
                2.5f);
    }
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(innerLeft + innerWidth * frac, barY + barH * 0.5f),
                                  5.0f, 5.0f),
                    textB);
  }

  auto clockText = [](double seconds) {
    if (seconds <= 0.0) return std::wstring(L"0:00");
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t buf[16]{};
    swprintf(buf, 16, L"%d:%02d", total / 60, total % 60);
    return std::wstring(buf);
  };

  const float timeY = barY + 11.0f;
  textAt(rt, tiny_, clockText(pos).c_str(),
         D2D1::RectF(innerLeft, timeY, innerLeft + 60.0f, timeY + 16.0f), mutedB);
  textAt(rt, tiny_, seekable ? clockText(dur).c_str() : L"--:--",
         D2D1::RectF(innerLeft + innerWidth - 60.0f, timeY, innerLeft + innerWidth,
                     timeY + 16.0f),
         mutedB, DWRITE_TEXT_ALIGNMENT_TRAILING);

  // Transport sits on the card's centre line; the volume block is pinned right.
  // At the panel's real width they clear each other by a wide gap, so no clamp
  // is needed - and clamping would pull the buttons off centre, which is the
  // one thing this layout exists to avoid.
  const float btnSize = 30.0f;
  const float gap = 12.0f;
  const float btnY = y + 94.0f;
  const float groupW = btnSize * 3.0f + gap * 2.0f;
  const float groupLeft = x + (w - groupW) * 0.5f;

  for (int i = 0; i < 3; ++i) {
    const float bx = groupLeft + static_cast<float>(i) * (btnSize + gap);
    const D2D1_RECT_F r = D2D1::RectF(bx, btnY, bx + btnSize, btnY + btnSize);

    fillRound(rt, i == 1 ? accentB : trackB, r, btnSize * 0.5f);

    ID2D1SolidColorBrush* glyph = nullptr;
    rt->CreateSolidColorBrush(i == 1 ? kNotchBg : kText, &glyph);

    const float cx = bx + btnSize * 0.5f;
    const float cy = btnY + btnSize * 0.5f;

    if (i == 0 || i == 2) {
      // Prev has its bar on the left and its apex pointing left; next mirrors it.
      const bool prev = (i == 0);
      const float barX = prev ? cx - 6.5f : cx + 4.5f;
      rt->FillRectangle(D2D1::RectF(barX, cy - 5.0f, barX + 2.0f, cy + 5.0f), glyph);

      ID2D1PathGeometry* geo = nullptr;
      ID2D1GeometrySink* sink = nullptr;
      if (d2d_ && SUCCEEDED(d2d_->CreatePathGeometry(&geo)) && geo && SUCCEEDED(geo->Open(&sink))) {
        const float back = prev ? 5.0f : -5.0f;   // wide edge, away from the apex
        const float apex = prev ? -4.5f : 4.5f;   // apex, toward the bar
        sink->BeginFigure(D2D1::Point2F(cx + back, cy - 5.5f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(cx + apex, cy));
        sink->AddLine(D2D1::Point2F(cx + back, cy + 5.5f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt->FillGeometry(geo, glyph);
      }
      if (sink) sink->Release();
      if (geo) geo->Release();
    } else if (media_.playing) {
      rt->FillRectangle(D2D1::RectF(cx - 4.5f, cy - 5.5f, cx - 1.5f, cy + 5.5f), glyph);
      rt->FillRectangle(D2D1::RectF(cx + 1.5f, cy - 5.5f, cx + 4.5f, cy + 5.5f), glyph);
    } else {
      ID2D1PathGeometry* geo = nullptr;
      ID2D1GeometrySink* sink = nullptr;
      if (d2d_ && SUCCEEDED(d2d_->CreatePathGeometry(&geo)) && geo && SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(cx - 4.0f, cy - 6.0f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(cx + 6.0f, cy));
        sink->AddLine(D2D1::Point2F(cx - 4.0f, cy + 6.0f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt->FillGeometry(geo, glyph);
      }
      if (sink) sink->Release();
      if (geo) geo->Release();
    }

    if (glyph) glyph->Release();
    mediaButtons_[i] = r;
  }
  mediaButtonsValid_ = true;

  drawVolumeBlock(rt, d2d_, tiny_, x + w - kCardPad, btnY + btnSize * 0.5f, volume_, muted_,
                  volumeAvailable_, &volumeRect_);

  if (accentB) accentB->Release();
  if (trackB) trackB->Release();
  if (mutedB) mutedB->Release();
  if (textB) textB->Release();
  if (tileB) tileB->Release();

  return kMediaH;
}

bool Scene::hitTestSeek(float x, float y, PanelMode mode) const {
  if (mode != PanelMode::Expanded || !media_.hasSession) return false;
  if (media_.durationSec <= 0.5) return false;
  return x >= progressRect_.left && x <= progressRect_.right && y >= progressRect_.top &&
         y <= progressRect_.bottom;
}

bool Scene::hitTestVolumeSlider(float x, float y, PanelMode mode) const {
  if (mode != PanelMode::Expanded || !volumeAvailable_) return false;
  return x >= volumeRect_.left && x <= volumeRect_.right && y >= volumeRect_.top &&
         y <= volumeRect_.bottom;
}

float Scene::fractionFromX(float x, float bar) const {
  const bool useSeek = bar > 0.5f;
  const D2D1_RECT_F& r = useSeek ? progressRect_ : volumeRect_;
  const float width = r.right - r.left;
  if (width <= 1.0f) return 0.0f;
  return std::clamp((x - r.left) / width, 0.0f, 1.0f);
}

int Scene::hitTestMedia(float x, float y, PanelMode mode) const {
  if (mode != PanelMode::Expanded || !mediaButtonsValid_) return -1;
  for (int i = 0; i < 3; ++i) {
    const D2D1_RECT_F& r = mediaButtons_[i];
    if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
  }
  return -1;
}

void Scene::drawPanel(ID2D1RenderTarget* rt, const Snapshot& s, float w, float h, float top) {
  const float pad = 16.0f;
  const float inner = w - pad * 2.0f;

  const float mediaHeight = drawMediaCard(rt, pad, top + kGridTop, inner, s);
  drawLauncherGrid(rt, pad, top + kGridTop + mediaHeight, inner);

  (void)h;
}

int Scene::hitTestLauncher(float x, float y, PanelMode mode) const {
  if (mode != PanelMode::Expanded) return -1;
  for (int i = 0; i < tileCount_; ++i) {
    const D2D1_RECT_F& r = tileRects_[i];
    if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
  }
  return -1;
}
