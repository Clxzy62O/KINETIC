// KINETIC desktop application (Qt 6 Widgets shell over the engine).
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

#include "MainWindow.h"

static void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor bg("#101114"), bg2("#17191E"), panel("#1D2027"), border("#30343D"), text("#F2F3F5"), text2("#A1A6B0"),
        accent("#A78BFA");
    p.setColor(QPalette::Window, bg2);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, bg);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, text2);
    p.setColor(QPalette::Disabled, QPalette::Text, text2);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, text2);
    p.setColor(QPalette::Mid, border);
    app.setPalette(p);
    app.setStyleSheet(
        "QDockWidget::title { background:#17191E; padding:5px 8px; color:#A1A6B0; }"
        "QGroupBox { border:1px solid #30343D; border-radius:6px; margin-top:14px; padding-top:6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left:8px; color:#A78BFA; }"
        "QPushButton { background:#1D2027; border:1px solid #30343D; border-radius:5px; padding:5px 10px; }"
        "QPushButton:hover { border-color:#A78BFA; }"
        "QToolBar { background:#17191E; border:none; spacing:4px; }"
        "QListWidget { border:none; }"
        "QMainWindow::separator { background:#30343D; width:2px; height:2px; }");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KINETIC");
    QApplication::setOrganizationName("KINETIC");
    applyTheme(app);
    MainWindow w;
    w.resize(1600, 960);
    w.show();
    if (argc > 1) w.openPath(QString::fromLocal8Bit(argv[1]));
    else w.showWelcome();
    return app.exec();
}
