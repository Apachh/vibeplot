#pragma once

#include "ElfModel.h"
#include "Poller.h"
#include "Series.h"
#include "StreamSink.h"

#include <QMainWindow>
#include <QMutex>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <memory>
#include <unordered_map>

class GraphTab;
class RegistersDialog;
class StreamDialog;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    bool loadElf(const QString &path, bool quiet = false);

    // ---- used by the tabs and the dialogs
    int newId() { return nextId_++; }
    // Call after any change that affects what the polling thread reads.
    void specsChanged();
    Poller *poller() const { return poller_; }
    const ElfModel *model() const { return model_.get(); }
    QString elfPath() const { return elfPath_; }
    void addRegisterSeries(const QString &name, quint64 addr, int size, std::optional<BitSpec> bits = std::nullopt,
                           const EnumMap &en = {});

    std::vector<Series *> allSeries() const;
    std::vector<GraphTab *> allTabs() const;
    GraphTab *currentTab() const;
    bool writeCsv(const QString &path);                // used by "Export CSV…"

    StreamSink *sink() const { return sink_.get(); }
    QString streamMessage() const { return streamMsg_; }
    QString startStream(const StreamConfig &cfg);     // returns an error text, empty on success
    void stopStream();

protected:
    void closeEvent(QCloseEvent *) override;

private:
    void buildUi();

    // ELF / variable tree
    void openElf();
    void reloadElf(bool automatic = false);
    void checkElf();
    QTreeWidgetItem *makeItem(Node *node);
    void onExpand(QTreeWidgetItem *item);
    void applyFilter(const QString &text);
    void addItems(const QList<QTreeWidgetItem *> &items);
    void addByAddress();

    // connection / sampling
    void toggleConnect();
    void onSample(double t, const Samples &values);
    std::vector<ReadSpec> specsSnapshot();

    // tabs
    GraphTab *addTab(const QString &title = {});
    void closeTab(int i);
    void renameTab(int i);

    // display
    void refresh();
    void setFollow(bool on);
    void fitView();
    void setCursors(bool on);
    void clearData();

    // dialogs
    void showRegisters();
    void showStream();
    void streamSample(double t, const QHash<int, Reading> &values);
    void updateStreamUi();
    void exportCsv();

    std::unique_ptr<ElfModel> model_;
    QString elfPath_;
    std::optional<std::pair<qint64, qint64>> elfSig_;       // (mtime ns, size)
    std::optional<std::pair<qint64, qint64>> elfPending_;

    // shared with the polling thread
    QMutex specsMu_;
    std::vector<ReadSpec> specs_;
    std::unordered_map<int, Series *> byId_;                // GUI thread only
    std::atomic<double> period_{0.01};

    QPointer<Poller> poller_;
    int nextId_ = 0;
    int tabNo_ = 0;
    double tLast_ = 0.0;
    bool follow_ = true;
    int tick_ = 0;

    RegistersDialog *regDlg_ = nullptr;
    StreamDialog *streamDlg_ = nullptr;
    std::unique_ptr<StreamSink> sink_;
    StreamConfig streamCfg_;
    QString streamMsg_;
    std::vector<int> streamCols_;
    bool streamColsValid_ = false;
    int streamN_ = 0;

    QTimer timer_;

    // widgets
    QPushButton *btnReload_ = nullptr, *btnConn_ = nullptr, *btnFollow_ = nullptr, *btnCursors_ = nullptr,
                *btnStream_ = nullptr;
    QCheckBox *chkAutoElf_ = nullptr;
    QLabel *lblElf_ = nullptr;
    QLineEdit *editHost_ = nullptr, *editFilter_ = nullptr;
    QSpinBox *spinPort_ = nullptr, *spinPeriod_ = nullptr;
    QDoubleSpinBox *spinWindow_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QTabWidget *tabs_ = nullptr;
};
