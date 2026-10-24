// A small live-plot widget: several curves, grid, nice axis ticks, optional enum labels on the
// Y axis, a legend, two draggable measurement cursors and mouse zoom / pan.
//
//   wheel            zoom both axes around the pointer (Ctrl: time only, Shift: value only)
//   left drag        pan (a drag that starts on a cursor line moves that cursor instead)
//   right drag       zoom, horizontal movement scales time, vertical movement scales value
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

#include <map>
#include <utility>
#include <vector>

class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget *parent = nullptr);

    // ---- curves
    int addCurve(const QString &name, const QColor &color);
    void removeCurve(int id);
    void setCurveVisible(int id, bool visible);
    void setCurveData(int id, std::vector<double> x, std::vector<double> y);

    // ---- view
    void setXRange(double lo, double hi, bool notify = true);
    std::pair<double, double> xRange() const { return {xlo_, xhi_}; }
    std::pair<double, double> yRange() const { return {ylo_, yhi_}; }
    // Y range follows the data (until the user zooms / pans vertically).
    void enableAutoY();
    // When auto-ranging Y, count only the data inside the current time range.
    void setAutoVisible(bool on);
    // Show the whole time range of all curves.
    void fitX();

    using Ticks = std::vector<std::pair<double, QString>>;
    void setEnumTicks(const Ticks &ticks);   // empty: numeric axis
    const Ticks &enumTicks() const { return enumTicks_; }

    // ---- cursors
    void setCursorsVisible(bool on);
    double cursorValue(int i) const { return cursors_[i].x; }
    void setCursorValue(int i, double x);
    void setTitle(const QString &text);

signals:
    void xRangeChanged(double lo, double hi);   // from interaction or setXRange(notify = true)
    void cursorMoved(int index, double x);      // from dragging

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    QSize sizeHint() const override { return {600, 300}; }

private:
    struct Curve {
        QString name;
        QColor color;
        bool visible = true;
        std::vector<double> x, y;
    };
    struct Cursor {
        double x = 0;
        QColor color;
        const char *label;
        double labelPos;      // 0 = bottom, 1 = top of the line
    };

    QRectF plotRect(const QFontMetrics &fm) const;
    double toPx(double x) const;
    double toPy(double y) const;
    double fromPx(double px) const;
    double fromPy(double py) const;
    void autoRangeNow();
    int cursorAt(const QPointF &p) const;
    void zoom(double fx, double fy, const QPointF &center, bool doX, bool doY);
    void drawCurve(QPainter &p, const Curve &c, const QRectF &r) const;

    std::map<int, Curve> curves_;
    int nextId_ = 0;
    double xlo_ = 0, xhi_ = 10, ylo_ = 0, yhi_ = 1;
    bool autoX_ = true, autoY_ = true, autoVisible_ = false;
    Ticks enumTicks_;
    Cursor cursors_[2];
    bool cursorsVisible_ = false;
    QString title_;

    QRectF rect_;               // plot area of the last paint
    enum class Drag { None, Pan, Zoom, Cursor } drag_ = Drag::None;
    int dragCursor_ = -1;
    QPointF last_, zoomAnchor_;
};
