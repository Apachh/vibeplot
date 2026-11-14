// SWD Live Plot
// =============
// Load a firmware .elf, pick variables from its debug info and plot them live while you debug
// with Cortex-Debug (ST-Link / OpenOCD). No firmware changes.
//
// How it works: Cortex-Debug starts OpenOCD and OpenOCD owns the ST-Link. This tool connects to
// OpenOCD's Tcl port and reads the variables' RAM addresses over SWD (background memory access,
// the CPU is not halted).
//
// Usage: swdplot [firmware.elf]
#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("swdplot");
    app.setApplicationName("swd_liveplot");
    applyDarkTheme(app);

    MainWindow w;
    w.resize(1400, 820);
    w.show();
    if (argc > 1) w.loadElf(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
