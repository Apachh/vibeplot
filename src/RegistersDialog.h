// MCU register viewer: peripherals from a CMSIS-SVD file plus the built-in Cortex-M core registers.
// Values are read through the same OpenOCD connection as the plot (Connect first).
#pragma once

#include "Poller.h"
#include "SvdRegs.h"

#include <QDialog>
#include <QSettings>
#include <QTimer>

#include <unordered_map>

class MainWindow;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

class RegistersDialog : public QDialog {
    Q_OBJECT
public:
    explicit RegistersDialog(MainWindow *main);

    // A different ELF was opened: switch to the SVD bound to it.
    void onElfLoaded(const QString &path);
    // Result of a read requested through refresh().
    void onRegs(quint64 token, const RegResults &res);
    bool openSvd(const QString &path, bool remember = true, bool quiet = false);

protected:
    void showEvent(QShowEvent *) override;

private:
    enum Kind { KindNone = 0, KindPeripheral, KindRegister, KindField };

    static QString settingsKey(const QString &path);
    void autoload();
    void browseSvd();
    void setPeripherals(std::vector<std::shared_ptr<svd::Peripheral>> periphs, const svd::DeviceInfo &info);

    QTreeWidgetItem *periphItem(const svd::Peripheral *p);
    QTreeWidgetItem *regItem(const svd::Register *r);
    QTreeWidgetItem *fieldItem(const svd::Field *f);
    static bool hasPlaceholder(const QTreeWidgetItem *it);
    void onExpand(QTreeWidgetItem *it);
    void applyFilter(const QString &text);

    std::vector<const svd::Register *> visibleRegs() const;
    void refresh(std::vector<const svd::Register *> regs = {}, bool force = false);
    void refreshSoon(const svd::Register *r);
    void tick();
    void setStatus(const QString &msg);
    void color(QTreeWidgetItem *it, int col, bool changed);
    void showValue(const svd::Register *r, QTreeWidgetItem *it, quint64 v, std::optional<quint64> prev);

    struct Owner {
        const svd::Peripheral *periph = nullptr;
        const svd::Register *reg = nullptr;
    };
    Owner ownerOf(QTreeWidgetItem *it) const;
    void onDouble(QTreeWidgetItem *it, int col);
    void onMenu(const QPoint &pos);
    void plot(const svd::Peripheral *p, const svd::Register *r, const svd::Field *f);
    bool confirmWrite(const QString &what);
    void writeRegister(const svd::Peripheral *p, const svd::Register *r);
    void writeField(const svd::Peripheral *p, const svd::Register *r, const svd::Field *f);

    MainWindow *main_;
    QString svdPath_;
    svd::DeviceInfo info_;
    std::vector<std::shared_ptr<svd::Peripheral>> periphs_;
    std::unordered_map<const svd::Register *, QTreeWidgetItem *> items_;
    std::unordered_map<const svd::Register *, quint64> values_;
    std::unordered_map<quint64, std::vector<const svd::Register *>> requests_;
    quint64 nextToken_ = 0;
    bool pending_ = false;
    qint64 pendingSince_ = 0;
    bool initialised_ = false;
    QString elfFor_;
    QSettings settings_;
    QTimer timer_;

    QPushButton *btnLoad_, *btnReload_, *btnRefresh_;
    QLabel *lblSvd_, *lblStatus_;
    QLineEdit *editFilter_;
    QCheckBox *chkAuto_;
    QSpinBox *spinMs_;
    QTreeWidget *tree_;
};
