#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
#include <QTimer>

#include <memory>

#include "Document.h"
#include "media/Decoder.h"

class TimelineWidget;
class ViewerWidget;
class VelocityGraph;
class MediaList;
class QComboBox;
class QDockWidget;
class QScrollArea;
class QLabel;
class QAudioSink;
class QBuffer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    void showWelcome();
    bool openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void buildUi();
    void buildMenus();
    void newProject(bool askSettings);
    void openProject();
    bool save();
    bool saveAs();
    bool maybeSave();
    void importMedia(const QStringList& paths);
    void exportVideo();
    void detectBeats();
    void cutAtBeats(bool strongOnly);
    void flashesOnStrongBeats();
    void shakeOnBeats();
    void splitAtPlayhead();
    void deleteSelected(bool ripple);
    void togglePlay();
    void stopPlayback();
    void playTick();
    void rebuildInspector();
    void refreshMediaList();
    void updateTitle();
    void applyWorkspace(const QString& name);
    void autosave();
    void openTutorial();
    void addRecent(const QString& path);

    Document doc_;
    kinetic::media::FrameProvider frames_{size_t(768) << 20};
    TimelineWidget* timeline_ = nullptr;
    ViewerWidget* viewer_ = nullptr;
    VelocityGraph* velocity_ = nullptr;
    MediaList* media_ = nullptr;
    QScrollArea* inspectorScroll_ = nullptr;
    QDockWidget *mediaDock_ = nullptr, *inspectorDock_ = nullptr, *timelineDock_ = nullptr, *velocityDock_ = nullptr;
    QComboBox* quality_ = nullptr;
    QLabel* status_ = nullptr;
    QString workspace_ = "Standard Edit";
    bool advanced_ = false;
    bool rebuildingInspector_ = false;

    QTimer playTimer_, autosaveTimer_;
    QElapsedTimer clock_;
    int64_t playStartFrame_ = 0;
    bool playing_ = false;
#ifdef KINETIC_HAS_AUDIO
    QAudioSink* sink_ = nullptr;
    QBuffer* audioBuf_ = nullptr;
#endif
};
