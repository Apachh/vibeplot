// One graph tab: its own plots (sharing the time axis), cursors, variable table and
// post-processing panel. Sampling and the OpenOCD connection are shared via MainWindow.
#pragma once

#include "PlotWidget.h"
#include "Series.h"

#include <QWidget>

#include <memory>
#include <vector>

class MainWindow;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QSplitter;
class QTableWidget;

class GraphTab : public QWidget {
    Q_OBJECT
public:
    explicit GraphTab(MainWindow *main);

    const std::vector<std::unique_ptr<Series>> &series() const { return series_; }

    // ---- series management
    Series *addSeries(const QString &name, quint64 addr, int size, char fmt, const EnumMap &en = {},
                      std::optional<BitSpec> bits = std::nullopt, bool fixed = false);
    void removeSeries(Series *s);
    void removeAllSeries();
    // After an ELF reload: look the variables up by name; returns (moved, missing).
    std::pair<int, int> remapSeries();

    // ---- display
    void refresh(bool cursors, bool follow, double window, double tLast);
    void setFollow(bool on);
    void fitX();
    void fitY();
    void setCursors(bool on);
    void clearData();

private:
    enum Col { Show, Name, Address, Type, Offset, Value, AtC1, AtC2, Delta, Remove, Graph, ColCount };

    // graphs inside the tab
    void addPlot();
    void removePlot();
    void plotsChanged();
    void syncCursor(PlotWidget *src, int index, double x);
    void syncRange(PlotWidget *src, double lo, double hi);
    QWidget *makeGraphWidget(Series *s);
    void moveSeries(Series *s, int plotIndex);
    void detachCurve(Series *s);

    // table
    int rowOf(const Series *s) const;
    void addRow(Series *s);
    QWidget *makeTypeWidget(Series *s);
    QWidget *makeOffsetWidget(Series *s);
    void setActive(Series *s, bool on);
    void setType(Series *s, const QString &typeName);
    void removeSelected();
    void setText(int row, int col, const QString &text);
    void setCell(int row, int col, std::optional<double> v);
    std::vector<Series *> selectedSeries() const;

    // value writing
    void editValue(int row = -1);

    // post-processing panel
    void configureParam(const QString &mode, std::optional<double> value = std::nullopt);
    void onSelection();
    void onModeChanged();
    void onPpChanged();
    void resetPp();

    void updateEnumAxis();

    MainWindow *main_;
    std::vector<std::unique_ptr<Series>> series_;
    std::vector<PlotWidget *> plots_;
    QSplitter *plotSplit_ = nullptr;
    QTableWidget *table_ = nullptr;
    QComboBox *cmbTarget_ = nullptr;
    QGroupBox *ppBox_ = nullptr;
    QLabel *lblSel_ = nullptr;
    QComboBox *cmbMode_ = nullptr;
    QDoubleSpinBox *spinParam_ = nullptr;
    QDoubleSpinBox *spinGain_ = nullptr;
    bool loading_ = false;
    bool cursorsOn_ = false;
    bool following_ = true;
    bool inRangeSync_ = false;
};
