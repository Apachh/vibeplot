#include "PlotWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const QColor kBackground("#1e1e1e");
const QColor kForeground("#d4d4d4");

double niceStep(double range, int targetTicks)
{
    const double raw = range / std::max(1, targetTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / mag;
    return (f < 1.5 ? 1.0 : f < 3.5 ? 2.0 : f < 7.5 ? 5.0 : 10.0) * mag;
}

QString tickText(double v, double step)
{
    if (std::fabs(v) < step * 1e-9) v = 0;
    const int dec = std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-9)), 0, 10);
    if (std::fabs(v) >= 1e7 || (v != 0 && std::fabs(v) < 1e-5)) return QString::number(v, 'g', 6);
    return QString::number(v, 'f', dec);
}

std::vector<double> ticksFor(double lo, double hi, int target, double &step)
{
    std::vector<double> t;
    if (!(hi > lo)) return t;
    step = niceStep(hi - lo, target);
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) t.push_back(v);
    return t;
}

}  // namespace

PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    cursors_[0] = {2.0, QColor("#ffd54f"), "C1", 0.95};
    cursors_[1] = {4.0, QColor("#4fc3f7"), "C2", 0.88};
}

// ---- curves
int PlotWidget::addCurve(const QString &name, const QColor &color)
{
    const int id = nextId_++;
    curves_[id] = Curve{name, color, true, {}, {}};
    update();
    return id;
}

void PlotWidget::removeCurve(int id)
{
    curves_.erase(id);
    update();
}

void PlotWidget::setCurveVisible(int id, bool visible)
{
    auto it = curves_.find(id);
    if (it != curves_.end()) it->second.visible = visible;
    update();
}

void PlotWidget::setCurveData(int id, std::vector<double> x, std::vector<double> y)
{
    auto it = curves_.find(id);
    if (it == curves_.end()) return;
    it->second.x = std::move(x);
    it->second.y = std::move(y);
    update();
}

// ---- view
void PlotWidget::setXRange(double lo, double hi, bool notify)
{
    if (!(hi > lo)) hi = lo + 1e-6;
    const bool changed = lo != xlo_ || hi != xhi_;
    xlo_ = lo;
    xhi_ = hi;
    autoX_ = false;
    if (changed) {
        update();
        if (notify) emit xRangeChanged(lo, hi);
    }
}

void PlotWidget::enableAutoY()
{
    autoY_ = true;
    update();
}

void PlotWidget::setAutoVisible(bool on)
{
    autoVisible_ = on;
    update();
}

void PlotWidget::fitX()
{
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &[id, c] : curves_) {
        if (!c.visible || c.x.empty()) continue;
        lo = std::min(lo, c.x.front());
        hi = std::max(hi, c.x.back());
    }
    if (lo > hi) return;
    if (hi <= lo) hi = lo + 1.0;
    const double pad = 0.02 * (hi - lo);
    setXRange(lo - pad, hi + pad);
}

void PlotWidget::setEnumTicks(const Ticks &ticks)
{
    enumTicks_ = ticks;
    update();
}

// ---- cursors
void PlotWidget::setCursorsVisible(bool on)
{
    cursorsVisible_ = on;
    update();
}

void PlotWidget::setCursorValue(int i, double x)
{
    cursors_[i].x = x;
    update();
}

void PlotWidget::setTitle(const QString &text)
{
    if (title_ == text) return;
    title_ = text;
    update();
}

// ---- geometry
QRectF PlotWidget::plotRect(const QFontMetrics &fm) const
{
    double labelW = 0;
    if (!enumTicks_.empty()) {
        for (const auto &[v, s] : enumTicks_) labelW = std::max<double>(labelW, fm.horizontalAdvance(s));
    } else {
        double step = 1;
        for (double v : ticksFor(ylo_, yhi_, 6, step)) labelW = std::max<double>(labelW, fm.horizontalAdvance(tickText(v, step)));
    }
    const double left = std::max(46.0, labelW + 14.0);
    const double top = title_.isEmpty() ? 8.0 : fm.height() + 12.0;
    const double bottom = fm.height() * 2 + 12.0;
    return QRectF(left, top, std::max(10.0, width() - left - 10.0), std::max(10.0, height() - top - bottom));
}

double PlotWidget::toPx(double x) const { return rect_.left() + (x - xlo_) / (xhi_ - xlo_) * rect_.width(); }
double PlotWidget::toPy(double y) const { return rect_.bottom() - (y - ylo_) / (yhi_ - ylo_) * rect_.height(); }
double PlotWidget::fromPx(double px) const { return xlo_ + (px - rect_.left()) / rect_.width() * (xhi_ - xlo_); }
double PlotWidget::fromPy(double py) const { return ylo_ + (rect_.bottom() - py) / rect_.height() * (yhi_ - ylo_); }

void PlotWidget::autoRangeNow()
{
    if (autoX_) {
        double lo = INFINITY, hi = -INFINITY;
        for (const auto &[id, c] : curves_) {
            if (!c.visible || c.x.empty()) continue;
            lo = std::min(lo, c.x.front());
            hi = std::max(hi, c.x.back());
        }
        if (lo <= hi) {
            if (hi <= lo) hi = lo + 1.0;
            const double pad = 0.02 * (hi - lo);
            xlo_ = lo - pad;
            xhi_ = hi + pad;
        }
    }
    if (!autoY_) return;
    double lo = INFINITY, hi = -INFINITY;
    for (const auto &[id, c] : curves_) {
        if (!c.visible || c.x.empty()) continue;
        size_t i0 = 0, i1 = c.x.size();
        if (autoVisible_) {
            i0 = std::lower_bound(c.x.begin(), c.x.end(), xlo_) - c.x.begin();
            i1 = std::upper_bound(c.x.begin(), c.x.end(), xhi_) - c.x.begin();
        }
        for (size_t i = i0; i < i1; ++i) {
            const double v = c.y[i];
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (lo > hi) return;
    if (hi - lo < 1e-12 * std::max(1.0, std::fabs(hi))) {
        const double d = std::max(0.5, std::fabs(hi) * 0.05);
        lo -= d;
        hi += d;
    }
    const double pad = 0.05 * (hi - lo);
    ylo_ = lo - pad;
    yhi_ = hi + pad;
}

// ---- painting
void PlotWidget::drawCurve(QPainter &p, const Curve &c, const QRectF &r) const
{
    if (c.x.empty()) return;
    const double span = xhi_ - xlo_;
    size_t i0 = std::lower_bound(c.x.begin(), c.x.end(), xlo_ - span * 0.01) - c.x.begin();
    size_t i1 = std::upper_bound(c.x.begin(), c.x.end(), xhi_ + span * 0.01) - c.x.begin();
    if (i0 > 0) --i0;                                   // keep the segment that enters the view
    if (i1 < c.x.size()) ++i1;
    if (i1 <= i0) return;

    const size_t n = i1 - i0;
    const size_t cols = static_cast<size_t>(std::max(1.0, r.width()));
    QPolygonF poly;
    if (n > cols * 4) {                                 // peak decimation: min and max per pixel column
        poly.reserve(static_cast<int>(cols * 2 + 2));
        size_t i = i0;
        while (i < i1) {
            const long col = std::lround(std::floor(toPx(c.x[i])));
            size_t lo = i, hi = i, j = i;
            for (; j < i1 && std::lround(std::floor(toPx(c.x[j]))) == col; ++j) {
                if (c.y[j] < c.y[lo]) lo = j;
                if (c.y[j] > c.y[hi]) hi = j;
            }
            const size_t a = std::min(lo, hi), b = std::max(lo, hi);
            poly << QPointF(toPx(c.x[a]), toPy(c.y[a]));
            if (b != a) poly << QPointF(toPx(c.x[b]), toPy(c.y[b]));
            i = j;
        }
    } else {
        poly.reserve(static_cast<int>(n));
        for (size_t i = i0; i < i1; ++i) poly << QPointF(toPx(c.x[i]), toPy(c.y[i]));
    }
    p.drawPolyline(poly);
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    p.setFont(font());
    const QFontMetrics fm(font());

    autoRangeNow();
    rect_ = plotRect(fm);
    const QRectF r = rect_;

    // grid + tick labels
    QColor grid = kForeground;
    grid.setAlphaF(0.3);
    QPen gridPen(grid, 1);
    p.setRenderHint(QPainter::Antialiasing, false);
    double stepX = 1, stepY = 1;
    const auto xt = ticksFor(xlo_, xhi_, std::max(2, static_cast<int>(r.width() / 90)), stepX);
    p.setPen(gridPen);
    for (double v : xt) p.drawLine(QPointF(std::round(toPx(v)), r.top()), QPointF(std::round(toPx(v)), r.bottom()));
    std::vector<std::pair<double, QString>> yt;
    if (!enumTicks_.empty()) {
        yt = enumTicks_;
    } else {
        for (double v : ticksFor(ylo_, yhi_, std::max(2, static_cast<int>(r.height() / 45)), stepY)) yt.push_back({v, tickText(v, stepY)});
    }
    for (const auto &[v, s] : yt) {
        const double py = toPy(v);
        if (py < r.top() - 1 || py > r.bottom() + 1) continue;
        p.setPen(gridPen);
        p.drawLine(QPointF(r.left(), std::round(py)), QPointF(r.right(), std::round(py)));
    }
    p.setPen(kForeground);
    p.drawRect(r);
    for (double v : xt) {
        const QString s = tickText(v, stepX);
        p.drawText(QPointF(toPx(v) - fm.horizontalAdvance(s) / 2.0, r.bottom() + fm.height() + 2), s);
    }
    for (const auto &[v, s] : yt) {
        const double py = toPy(v);
        if (py < r.top() - 1 || py > r.bottom() + 1) continue;
        p.drawText(QPointF(r.left() - 6 - fm.horizontalAdvance(s), py + fm.ascent() / 2.0 - 1), s);
    }
    const QString xlabel = QStringLiteral("time (s)");
    p.drawText(QPointF(r.center().x() - fm.horizontalAdvance(xlabel) / 2.0, height() - 6), xlabel);
    if (!title_.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(10);
        p.setFont(f);
        p.drawText(QRectF(r.left(), 2, r.width(), fm.height() + 6), Qt::AlignCenter, title_);
        p.setFont(font());
    }

    // curves
    p.save();
    p.setClipRect(r);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &[id, c] : curves_) {
        if (!c.visible) continue;
        p.setPen(QPen(c.color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        drawCurve(p, c, r);
    }

    // cursors
    if (cursorsVisible_) {
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const Cursor &c : cursors_) {
            const double px = toPx(c.x);
            if (px < r.left() - 1 || px > r.right() + 1) continue;
            p.setPen(QPen(c.color, 2));
            p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
            const QString s = QStringLiteral("%1 %2s").arg(c.label).arg(c.x, 0, 'f', 3);
            const double ty = r.bottom() - c.labelPos * r.height() + fm.ascent() / 2.0;
            p.drawText(QPointF(px + 4, ty), s);
        }
    }
    p.restore();

    // legend
    if (!curves_.empty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        double w = 0;
        for (const auto &[id, c] : curves_) w = std::max<double>(w, fm.horizontalAdvance(c.name));
        const double rowH = fm.height() + 3, boxW = w + 40, boxH = rowH * curves_.size() + 8;
        const QRectF box(r.left() + 12, r.top() + 12, boxW, boxH);
        QColor bg = kBackground;
        bg.setAlpha(190);
        p.setPen(QColor(100, 100, 100));
        p.setBrush(bg);
        p.drawRoundedRect(box, 3, 3);
        double y = box.top() + 4;
        for (const auto &[id, c] : curves_) {
            QColor col = c.color;
            if (!c.visible) col.setAlphaF(0.35);
            p.setPen(QPen(col, 3));
            p.drawLine(QPointF(box.left() + 6, y + rowH / 2), QPointF(box.left() + 24, y + rowH / 2));
            p.setPen(c.visible ? kForeground : QColor(120, 120, 120));
            p.drawText(QPointF(box.left() + 30, y + fm.ascent()), c.name);
            y += rowH;
        }
    }
}

// ---- interaction
int PlotWidget::cursorAt(const QPointF &pt) const
{
    if (!cursorsVisible_ || !rect_.contains(pt)) return -1;
    int best = -1;
    double bestD = 6.0;
    for (int i = 0; i < 2; ++i) {
        const double d = std::fabs(toPx(cursors_[i].x) - pt.x());
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void PlotWidget::zoom(double fx, double fy, const QPointF &c, bool doX, bool doY)
{
    if (doX) {
        const double x = fromPx(c.x());
        const double lo = x - (x - xlo_) * fx, hi = x + (xhi_ - x) * fx;
        setXRange(lo, hi);
    }
    if (doY) {
        const double y = fromPy(c.y());
        ylo_ = y - (y - ylo_) * fy;
        yhi_ = y + (yhi_ - y) * fy;
        autoY_ = false;
    }
    update();
}

void PlotWidget::wheelEvent(QWheelEvent *e)
{
    if (!rect_.contains(e->position())) return;
    const double f = std::pow(0.85, e->angleDelta().y() / 120.0);
    const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
    zoom(f, f, e->position(), !shift, !ctrl);
    e->accept();
}

void PlotWidget::mousePressEvent(QMouseEvent *e)
{
    last_ = e->position();
    if (e->button() == Qt::LeftButton) {
        dragCursor_ = cursorAt(e->position());
        drag_ = dragCursor_ >= 0 ? Drag::Cursor : (rect_.contains(e->position()) ? Drag::Pan : Drag::None);
    } else if (e->button() == Qt::RightButton && rect_.contains(e->position())) {
        drag_ = Drag::Zoom;
        zoomAnchor_ = e->position();
    }
}

void PlotWidget::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    if (drag_ == Drag::None) {
        setCursor(cursorAt(pos) >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    const QPointF d = pos - last_;
    last_ = pos;
    if (drag_ == Drag::Cursor) {
        cursors_[dragCursor_].x = fromPx(pos.x());
        emit cursorMoved(dragCursor_, cursors_[dragCursor_].x);
        update();
    } else if (drag_ == Drag::Pan) {
        const double dx = -d.x() / rect_.width() * (xhi_ - xlo_);
        const double dy = d.y() / rect_.height() * (yhi_ - ylo_);
        autoY_ = false;
        ylo_ += dy;
        yhi_ += dy;
        setXRange(xlo_ + dx, xhi_ + dx);
        update();
    } else if (drag_ == Drag::Zoom) {
        zoom(std::exp(-d.x() * 0.01), std::exp(d.y() * 0.01), zoomAnchor_, true, true);
    }
}

void PlotWidget::mouseReleaseEvent(QMouseEvent *)
{
    drag_ = Drag::None;
    dragCursor_ = -1;
}
