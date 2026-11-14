// Dialog to start / stop streaming the plotted values to a virtual COM port, serial port or file.
#pragma once

#include <QDialog>

class MainWindow;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class StreamDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamDialog(MainWindow *main);
    void updateStatus();

private:
    void onTarget(int i);
    void browse();
    void toggle();

    MainWindow *main_;
    QComboBox *cmbTarget_, *cmbFormat_, *cmbValues_, *cmbTs_;
    QLineEdit *editPath_;
    QPushButton *btnBrowse_, *btnGo_;
    QSpinBox *spinBaud_, *spinEvery_;
    QCheckBox *chkAppend_;
    QLabel *lblStatus_;
};
