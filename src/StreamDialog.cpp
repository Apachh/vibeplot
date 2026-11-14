#include "StreamDialog.h"

#include "MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
const QStringList kTargets = {"Virtual COM port (created by this app, Linux/macOS)",
                              "Existing serial port / COM port (e.g. one end of a com0com pair)", "File"};
const QStringList kDefaultPaths = {"/tmp/swdplot_com", "", "swd_stream.csv"};
}  // namespace

StreamDialog::StreamDialog(MainWindow *main) : QDialog(main), main_(main)
{
    setWindowTitle("Stream plotted data");
    setMinimumWidth(580);
    cmbTarget_ = new QComboBox;
    cmbTarget_->addItems(kTargets);
    editPath_ = new QLineEdit(kDefaultPaths[0]);
    btnBrowse_ = new QPushButton(QString::fromUtf8("Browse…"));
    connect(btnBrowse_, &QPushButton::clicked, this, &StreamDialog::browse);
    auto *prow = new QHBoxLayout;
    prow->addWidget(editPath_, 1);
    prow->addWidget(btnBrowse_);
    spinBaud_ = new QSpinBox;
    spinBaud_->setRange(300, 4000000);
    spinBaud_->setValue(115200);
    chkAppend_ = new QCheckBox("Append if the file already exists");
    cmbFormat_ = new QComboBox;
    cmbFormat_->addItems({"CSV (one row per sample)", "Teleplot (>name:value lines)", "JSON lines"});
    cmbValues_ = new QComboBox;
    cmbValues_->addItems({"Raw memory values", QString::fromUtf8("Raw × gain + offset (as set in the table)")});
    cmbTs_ = new QComboBox;
    cmbTs_->addItems({"Seconds since Connect", "Unix time (UTC seconds)", "No timestamp"});
    spinEvery_ = new QSpinBox;
    spinEvery_->setRange(1, 100000);
    spinEvery_->setPrefix("every ");
    spinEvery_->setSuffix(" sample(s)");

    auto *form = new QFormLayout;
    form->addRow("Output", cmbTarget_);
    form->addRow("Path / port", prow);
    form->addRow("Baud rate", spinBaud_);
    form->addRow("", chkAppend_);
    form->addRow("Format", cmbFormat_);
    form->addRow("Values", cmbValues_);
    form->addRow("Timestamp", cmbTs_);
    form->addRow("Send", spinEvery_);
    lblStatus_ = new QLabel("Not streaming");
    lblStatus_->setWordWrap(true);
    btnGo_ = new QPushButton("Start streaming");
    connect(btnGo_, &QPushButton::clicked, this, &StreamDialog::toggle);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(lblStatus_);
    lay->addWidget(btnGo_);
    connect(cmbTarget_, &QComboBox::currentIndexChanged, this, &StreamDialog::onTarget);
    onTarget(0);
}

void StreamDialog::onTarget(int i)
{
    if (kDefaultPaths.contains(editPath_->text().trimmed())) editPath_->setText(kDefaultPaths[i]);
    editPath_->setPlaceholderText(i == 1 ? "/dev/ttyUSB0, /dev/pts/5 or COM10" : "");
    spinBaud_->setEnabled(i == 1);
    btnBrowse_->setEnabled(i == 2);
    chkAppend_->setEnabled(i == 2);
}

void StreamDialog::browse()
{
    const QString path = QFileDialog::getSaveFileName(this, "Stream to file", editPath_->text(), "All files (*)");
    if (!path.isEmpty()) editPath_->setText(path);
}

void StreamDialog::toggle()
{
    if (main_->sink()) {
        main_->stopStream();
        updateStatus();
        return;
    }
    const QString path = editPath_->text().trimmed();
    if (path.isEmpty()) {
        QMessageBox::warning(this, "Stream", "Enter a path or port name.");
        return;
    }
    StreamConfig cfg;
    cfg.target = static_cast<StreamSink::Target>(cmbTarget_->currentIndex());
    cfg.path = path;
    cfg.baud = spinBaud_->value();
    cfg.append = chkAppend_->isChecked();
    cfg.format = static_cast<stream::Format>(cmbFormat_->currentIndex());
    cfg.scaled = cmbValues_->currentIndex() == 1;
    cfg.time = static_cast<stream::TimeBase>(cmbTs_->currentIndex());
    cfg.every = spinEvery_->value();
    if (cfg.target == StreamSink::File && !cfg.append && QFile::exists(path) && QFile(path).size() > 0) {
        if (QMessageBox::question(this, "Overwrite file?", path + " already has data. Overwrite it?") != QMessageBox::Yes) return;
    }
    const QString err = main_->startStream(cfg);
    if (!err.isEmpty()) QMessageBox::warning(this, "Cannot start streaming", err);
    updateStatus();
}

void StreamDialog::updateStatus()
{
    const StreamSink *sink = main_->sink();
    btnGo_->setText(sink ? "Stop streaming" : "Start streaming");
    for (QWidget *w : std::initializer_list<QWidget *>{cmbTarget_, editPath_, cmbFormat_, cmbValues_, cmbTs_, spinEvery_})
        w->setEnabled(!sink);
    if (!sink) {
        onTarget(cmbTarget_->currentIndex());
        lblStatus_->setText(main_->streamMessage().isEmpty() ? QStringLiteral("Not streaming") : main_->streamMessage());
    } else {
        for (QWidget *w : std::initializer_list<QWidget *>{spinBaud_, btnBrowse_, chkAppend_}) w->setEnabled(false);
        const QString hint = sink->device().isEmpty() ? QString() : QStringLiteral("\nOpen this path in the other program (any baud rate).");
        lblStatus_->setText(sink->describe() + hint);
    }
}
