#include "MainWindow.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QIcon>
#include <QPixmap>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

#ifdef KINETIC_HAS_AUDIO
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>
#endif

#include "Demo.h"
#include "Panels.h"
#include "TimelineWidget.h"
#include "audio/BeatDetect.h"
#include "edit/BeatSync.h"
#include "edit/Edit.h"
#include "export/Exporter.h"
#include "io/Storage.h"
#include "media/AudioDecode.h"
#include "media/Probe.h"
#include "project/Velocity.h"
#include "render/Effects.h"

using namespace kinetic;

static QString qs(const std::string& s) { return QString::fromStdString(s); }
static std::string ss(const QString& s) { return s.toStdString(); }

MainWindow::MainWindow() {
    setAcceptDrops(true);
    buildUi();
    buildMenus();
    connect(&doc_, &Document::changed, this, [this] {
        updateTitle();
        refreshMediaList();
        // Rebuild deferred (never delete the widget whose signal is running) and not while the
        // user is typing into one of the inspector's fields.
        QWidget* f = QApplication::focusWidget();
        bool typing = f && inspectorScroll_->isAncestorOf(f) && (qobject_cast<QAbstractSpinBox*>(f) || qobject_cast<QLineEdit*>(f));
        if (!typing) QTimer::singleShot(0, this, &MainWindow::rebuildInspector);
    });
    connect(&doc_, &Document::selectionChanged, this, [this] { QTimer::singleShot(0, this, &MainWindow::rebuildInspector); });
    connect(&doc_, &Document::playheadChanged, this, [this] { if (!playing_) timeline_->ensurePlayheadVisible(); });
    connect(&doc_, &Document::message, this, [this](const QString& m) { statusBar()->showMessage(m, 5000); });
    connect(&playTimer_, &QTimer::timeout, this, &MainWindow::playTick);
    connect(&autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer_.start(120 * 1000);
    newProject(false);
    QSettings st;
    restoreGeometry(st.value("window/geometry").toByteArray());
    applyWorkspace(st.value("window/workspace", "Standard Edit").toString());
}

MainWindow::~MainWindow() { stopPlayback(); }

void MainWindow::buildUi() {
    viewer_ = new ViewerWidget(&doc_, &frames_);
    setCentralWidget(viewer_);

    media_ = new MediaList;
    media_->setDragEnabled(true);
    media_->setIconSize(QSize(96, 54));
    mediaDock_ = new QDockWidget("Media", this);
    mediaDock_->setObjectName("media");
    mediaDock_->setWidget(media_);
    addDockWidget(Qt::LeftDockWidgetArea, mediaDock_);

    inspectorScroll_ = new QScrollArea;
    inspectorScroll_->setWidgetResizable(true);
    inspectorDock_ = new QDockWidget("Inspector", this);
    inspectorDock_->setObjectName("inspector");
    inspectorDock_->setWidget(inspectorScroll_);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock_);

    timeline_ = new TimelineWidget(&doc_);
    timelineDock_ = new QDockWidget("Timeline", this);
    timelineDock_->setObjectName("timeline");
    timelineDock_->setWidget(timeline_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);

    velocity_ = new VelocityGraph(&doc_);
    velocityDock_ = new QDockWidget("Velocity", this);
    velocityDock_->setObjectName("velocity");
    velocityDock_->setWidget(velocity_);
    addDockWidget(Qt::BottomDockWidgetArea, velocityDock_);
    splitDockWidget(timelineDock_, velocityDock_, Qt::Horizontal);

    auto* tb = addToolBar("Transport");
    tb->setObjectName("transport");
    tb->addAction("⏮", [this] { doc_.setPlayhead(0); });
    tb->addAction("◀", [this] { doc_.setPlayhead(doc_.playhead - 1); });
    tb->addAction("▶ / ❚❚", [this] { togglePlay(); });
    tb->addAction("▶", [this] { doc_.setPlayhead(doc_.playhead + 1); });
    tb->addSeparator();
    tb->addWidget(new QLabel(" Preview "));
    quality_ = new QComboBox;
    quality_->addItems({"Full", "Half", "Quarter"});
    quality_->setCurrentIndex(1);
    connect(quality_, &QComboBox::currentIndexChanged, this, [this](int i) { viewer_->setPreviewScale(i == 0 ? 1.0 : i == 1 ? 0.5 : 0.25); });
    tb->addWidget(quality_);
    tb->addSeparator();
    auto* ws = new QComboBox;
    ws->addItems({"Standard Edit", "Velocity Studio", "Effects Studio", "Audio Studio"});
    connect(ws, &QComboBox::textActivated, this, &MainWindow::applyWorkspace);
    tb->addWidget(new QLabel(" Workspace "));
    tb->addWidget(ws);
    status_ = new QLabel;
    statusBar()->addPermanentWidget(status_);
}

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu("&File");
    file->addAction("New Project…", QKeySequence::New, [this] { if (maybeSave()) newProject(true); });
    file->addAction("Open Project…", QKeySequence::Open, [this] { if (maybeSave()) openProject(); });
    file->addAction("Open Tutorial Project", [this] { if (maybeSave()) openTutorial(); });
    file->addSeparator();
    file->addAction("Save", QKeySequence::Save, [this] { save(); });
    file->addAction("Save As…", QKeySequence::SaveAs, [this] { saveAs(); });
    file->addSeparator();
    file->addAction("Import Media…", QKeySequence("Ctrl+I"), [this] {
        importMedia(QFileDialog::getOpenFileNames(this, "Import media", {}, "Media (*.mp4 *.mkv *.mov *.webm *.avi *.m4v *.png *.jpg *.jpeg *.webp *.wav *.mp3 *.flac *.m4a *.ogg *.aac);;All files (*)"));
    });
    file->addAction("Export…", QKeySequence("Ctrl+M"), [this] { exportVideo(); });
    file->addSeparator();
    file->addAction("Quit", QKeySequence::Quit, [this] { close(); });

    auto* editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("Undo", QKeySequence::Undo, [this] { doc_.undo(); });
    editMenu->addAction("Redo", QKeySequence("Ctrl+Shift+Z"), [this] { doc_.redo(); });
    editMenu->addSeparator();
    editMenu->addAction("Split at Playhead", QKeySequence("S"), [this] { splitAtPlayhead(); });
    editMenu->addAction("Delete", QKeySequence::Delete, [this] { deleteSelected(false); });
    editMenu->addAction("Ripple Delete", QKeySequence("Shift+Del"), [this] { deleteSelected(true); });
    editMenu->addAction("Duplicate After", QKeySequence("Ctrl+D"), [this] {
        if (const Clip* c = doc_.selected()) doc_.tryEdit([&] { return edit::duplicate(doc_.project, c->id, c->end()); });
        doc_.endGesture();
    });
    editMenu->addSeparator();
    editMenu->addAction("Add Adjustment Layer at Playhead", [this] {
        auto* c = doc_.comp();
        Id track = kNoId;
        for (auto it = c->tracks.rbegin(); it != c->tracks.rend(); ++it)
            if (it->kind == TrackKind::Video && edit::rangeFree(*it, doc_.playhead, static_cast<int64_t>(c->fps.toDouble() * 2))) { track = it->id; break; }
        if (!track) { statusBar()->showMessage("No free video track at the playhead", 4000); return; }
        Clip a;
        a.id = doc_.project.nextId;
        a.name = "Adjustment";
        a.kind = ClipKind::Adjustment;
        a.start = doc_.playhead;
        a.duration = static_cast<int64_t>(c->fps.toDouble() * 2);
        auto m = std::make_unique<MacroCommand>("Add Adjustment Layer");
        m->add(std::make_unique<SetNextIdCmd>(doc_.project.nextId, doc_.project.nextId + 1));
        m->add(std::make_unique<InsertClipCmd>(track, a));
        if (doc_.push(std::move(m))) doc_.select(a.id);
        doc_.endGesture();
    });
    editMenu->addAction("Set Composition Duration…", [this] {
        auto* c = doc_.comp();
        bool ok = false;
        double secs = QInputDialog::getDouble(this, "Duration", "Seconds:", c->durationSeconds(), 1, 36000, 2, &ok);
        if (ok) { c->durationFrames = secondsToNearestFrame(secs, c->fps); doc_.stack.clear(); emit doc_.changed(); }
    });

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Play / Pause", QKeySequence("Space"), [this] { togglePlay(); });
    view->addAction("Previous Frame", QKeySequence("Left"), [this] { doc_.setPlayhead(doc_.playhead - 1); });
    view->addAction("Next Frame", QKeySequence("Right"), [this] { doc_.setPlayhead(doc_.playhead + 1); });
    view->addAction("Go to Start", QKeySequence("Home"), [this] { doc_.setPlayhead(0); });
    view->addAction("Zoom In Timeline", QKeySequence("Ctrl+="), [this] { timeline_->zoomBy(1.5); });
    view->addAction("Zoom Out Timeline", QKeySequence("Ctrl+-"), [this] { timeline_->zoomBy(1 / 1.5); });
    view->addSeparator();
    for (const char* w : {"Standard Edit", "Velocity Studio", "Effects Studio", "Audio Studio"})
        view->addAction(w, [this, w] { applyWorkspace(w); });
    view->addAction("Reset Workspace Layout", [this] {
        QSettings().remove("layout/" + workspace_);
        QString w = workspace_;
        workspace_.clear();
        applyWorkspace(w);
    });

    auto* audio = menuBar()->addMenu("&Beat Sync");
    audio->addAction("Detect Beats in Music Track", QKeySequence("B"), [this] { detectBeats(); });
    audio->addAction("Cut Selected Clip at Beats", [this] { cutAtBeats(false); });
    audio->addAction("Cut Selected Clip at Strong Beats", [this] { cutAtBeats(true); });
    audio->addAction("White Flashes on Strong Beats", [this] { flashesOnStrongBeats(); });
    audio->addAction("Shake Selected Clip on Beats", [this] { shakeOnBeats(); });
}

// ---------------------------------------------------------------- project lifecycle
void MainWindow::newProject(bool askSettings) {
    QString name = "Untitled";
    int w = 1920, h = 1080;
    Rational fps(60, 1);
    double secs = 60;
    if (askSettings) {
        QDialog d(this);
        d.setWindowTitle("New Project");
        auto* f = new QFormLayout(&d);
        auto* nameE = new QLineEdit(name);
        auto* aspect = new QComboBox;
        aspect->addItems({"16:9 Landscape", "9:16 Vertical", "1:1 Square", "4:5 Portrait", "Custom"});
        auto* wS = new QSpinBox; wS->setRange(16, 8192); wS->setValue(w);
        auto* hS = new QSpinBox; hS->setRange(16, 8192); hS->setValue(h);
        auto* fpsC = new QComboBox;
        fpsC->addItems({"23.976", "24", "29.97", "30", "59.94", "60"});
        fpsC->setCurrentText("60");
        auto* dur = new QDoubleSpinBox; dur->setRange(1, 36000); dur->setValue(secs); dur->setSuffix(" s");
        connect(aspect, &QComboBox::currentIndexChanged, &d, [=](int i) {
            const int dims[4][2] = {{1920, 1080}, {1080, 1920}, {1080, 1080}, {1080, 1350}};
            if (i < 4) { wS->setValue(dims[i][0]); hS->setValue(dims[i][1]); }
        });
        f->addRow("Name", nameE);
        f->addRow("Format", aspect);
        f->addRow("Width", wS);
        f->addRow("Height", hS);
        f->addRow("Frame rate", fpsC);
        f->addRow("Duration", dur);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        f->addRow(bb);
        if (d.exec() != QDialog::Accepted) return;
        name = nameE->text();
        w = wS->value() / 2 * 2;
        h = hS->value() / 2 * 2;
        fps = Rational::parse(ss(fpsC->currentText()));
        secs = dur->value();
    }
    Project p;
    p.settings.name = ss(name);
    Composition c = makeComposition(p, "Main", w, h, fps, secondsToNearestFrame(secs, fps));
    c.background = {0.063, 0.067, 0.078, 1};
    p.rootCompositionId = c.id;
    p.compositions.push_back(c);
    auto& comp = p.compositions.back();
    addTrack(p, comp, TrackKind::Video, "V1");
    addTrack(p, comp, TrackKind::Video, "V2");
    addTrack(p, comp, TrackKind::Video, "V3 FX");
    addTrack(p, comp, TrackKind::Audio, "Music");
    addTrack(p, comp, TrackKind::Audio, "SFX");
    doc_.replaceProject(std::move(p), {});
    frames_.cache().clear();
    updateTitle();
}

bool MainWindow::openPath(const QString& path) {
    try {
        std::string file = ss(path);
        auto rec = io::checkRecovery(file);
        std::vector<std::string> warnings;
        Project p;
        bool recovered = false;
        if (rec.available &&
            QMessageBox::question(this, "Recover unsaved work?",
                                  "KINETIC found an autosave that is newer than this project (probably from a crash).\n"
                                  "Open the recovered version? The saved project file is not modified until you save.") == QMessageBox::Yes) {
            p = io::loadProject(rec.path, &warnings);
            recovered = true;
        } else {
            p = io::loadProject(file, &warnings);
        }
        doc_.replaceProject(std::move(p), path);
        if (recovered) doc_.stack.markDirty();  // recovered state is unsaved until the user saves
        frames_.cache().clear();
        for (auto& m : doc_.project.media)
            if (m.hasAudio && !m.hasVideo && QFileInfo::exists(qs(m.path))) {
                auto buf = media::decodeAudio(m.path, 8000, 1);
                timeline_->setWaveform(m.id, media::waveformPeaks(buf.samples, 6000), buf.duration());
            }
        addRecent(path);
        updateTitle();
        if (!warnings.empty()) {
            QString w;
            for (auto& s : warnings) w += "• " + qs(s) + "\n";
            QMessageBox::warning(this, "Project opened with warnings", w);
        }
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Could not open project", qs(e.what()));
        return false;
    }
}

void MainWindow::openProject() {
    QString f = QFileDialog::getOpenFileName(this, "Open project", {}, "KINETIC projects (*.kinetic)");
    if (!f.isEmpty()) openPath(f);
}

bool MainWindow::save() {
    if (doc_.path.isEmpty()) return saveAs();
    try {
        io::saveProject(doc_.project, ss(doc_.path));
        doc_.stack.markClean();
        addRecent(doc_.path);
        updateTitle();
        statusBar()->showMessage("Saved " + doc_.path, 3000);
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Save failed", qs(e.what()) + "\n\nYour previous saved file was not modified.");
        return false;
    }
}

bool MainWindow::saveAs() {
    QString f = QFileDialog::getSaveFileName(this, "Save project", qs(doc_.project.settings.name) + ".kinetic", "KINETIC projects (*.kinetic)");
    if (f.isEmpty()) return false;
    if (!f.endsWith(".kinetic")) f += ".kinetic";
    doc_.path = f;
    return save();
}

bool MainWindow::maybeSave() {
    if (!doc_.stack.isDirty()) return true;
    auto r = QMessageBox::question(this, "Unsaved changes", "Save changes to the current project?",
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return save();
    if (r == QMessageBox::Discard) { if (!doc_.path.isEmpty()) io::discardAutosave(ss(doc_.path)); return true; }
    return false;
}

void MainWindow::autosave() {
    if (doc_.path.isEmpty() || !doc_.stack.isDirty()) return;
    try {
        io::writeAutosave(doc_.project, ss(doc_.path));
        statusBar()->showMessage("Autosaved", 2000);
    } catch (const std::exception& e) {
        statusBar()->showMessage(QString("Autosave skipped: ") + e.what(), 5000);
    }
}

void MainWindow::addRecent(const QString& path) {
    QSettings st;
    QStringList r = st.value("recent").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 10) r.removeLast();
    st.setValue("recent", r);
}

void MainWindow::updateTitle() {
    QString name = doc_.path.isEmpty() ? qs(doc_.project.settings.name) : QFileInfo(doc_.path).fileName();
    setWindowTitle(QString("%1%2 — KINETIC").arg(name).arg(doc_.stack.isDirty() ? QStringLiteral(" •") : QString()));
    if (auto* c = doc_.comp())
        status_->setText(QString("%1×%2  %3 fps  %4 s  |  undo: %5")
                             .arg(c->width).arg(c->height).arg(c->fps.toDouble(), 0, 'f', 3).arg(c->durationSeconds(), 0, 'f', 1)
                             .arg(doc_.stack.canUndo() ? qs(doc_.stack.undoLabel()) : "—"));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!maybeSave()) { e->ignore(); return; }
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("layout/" + workspace_, saveState());
    st.setValue("window/workspace", workspace_);
    e->accept();
}

void MainWindow::openTutorial() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tutorial";
    QProgressDialog pd("Generating the tutorial project (media, music, beat analysis)…", {}, 0, 0, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.show();
    QApplication::processEvents();
    try {
        cli::runDemo(ss(dir), 12, 1280, 720, false);
        openPath(dir + "/demo.kinetic");
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Tutorial project", qs(e.what()));
    }
}

void MainWindow::showWelcome() {
    QDialog d(this);
    d.setWindowTitle("Welcome to KINETIC");
    d.setMinimumWidth(520);
    auto* v = new QVBoxLayout(&d);
    auto* title = new QLabel("<span style='font-size:28px;font-weight:700;color:#A78BFA'>KINETIC</span>"
                             "<br><span style='color:#A1A6B0'>The AMV editor</span>");
    v->addWidget(title);
    int choice = 0;
    auto button = [&](const QString& text, int id) {
        auto* b = new QPushButton(text);
        connect(b, &QPushButton::clicked, &d, [&, id] { choice = id; d.accept(); });
        v->addWidget(b);
    };
    button("New Project", 1);
    button("Open Project…", 2);
    button("Import Media into a New Project…", 3);
    button("Open Tutorial Project", 4);
    QStringList recent = QSettings().value("recent").toStringList();
    QString picked;
    if (!recent.isEmpty()) {
        v->addWidget(new QLabel("Recent projects"));
        auto* list = new QListWidget;
        list->addItems(recent);
        connect(list, &QListWidget::itemActivated, &d, [&](QListWidgetItem* it) { picked = it->text(); choice = 5; d.accept(); });
        v->addWidget(list);
    }
    d.exec();
    switch (choice) {
    case 1: newProject(true); break;
    case 2: openProject(); break;
    case 3: newProject(false); importMedia(QFileDialog::getOpenFileNames(this, "Import media")); break;
    case 4: openTutorial(); break;
    case 5: openPath(picked); break;
    default: break;
    }
}

// ---------------------------------------------------------------- media
void MainWindow::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (auto& u : e->mimeData()->urls()) if (u.isLocalFile()) paths << u.toLocalFile();
    importMedia(paths);
}

void MainWindow::importMedia(const QStringList& paths) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList failed;
    for (const QString& path : paths) {
        try {
            MediaRef m = media::probe(ss(path));
            m.id = doc_.project.nextId;
            auto macro = std::make_unique<MacroCommand>("Import " + m.name);
            macro->add(std::make_unique<SetNextIdCmd>(doc_.project.nextId, doc_.project.nextId + 1));
            macro->add(std::make_unique<AddMediaCmd>(m));
            doc_.push(std::move(macro));
            doc_.endGesture();
            if (m.hasAudio && !m.hasVideo) {
                auto buf = media::decodeAudio(m.path, 8000, 1);
                timeline_->setWaveform(m.id, media::waveformPeaks(buf.samples, 6000), buf.duration());
            }
        } catch (const std::exception& e) {
            failed << QFileInfo(path).fileName() + ": " + qs(e.what());
        }
    }
    QApplication::restoreOverrideCursor();
    if (!failed.isEmpty()) QMessageBox::warning(this, "Some files could not be imported", failed.join("\n"));
}

void MainWindow::refreshMediaList() {
    if (media_->count() == static_cast<int>(doc_.project.media.size())) return;
    media_->clear();
    for (auto& m : doc_.project.media) {
        QString info;
        if (m.hasVideo) info = QString("%1×%2  %3 fps  %4 s").arg(m.width).arg(m.height).arg(m.fps.toDouble(), 0, 'f', 3).arg(m.duration, 0, 'f', 1);
        else info = QString("audio  %1 Hz  %2 s").arg(m.sampleRate).arg(m.duration, 0, 'f', 1);
        auto* it = new QListWidgetItem(qs(m.name) + "\n" + info);
        it->setData(Qt::UserRole, static_cast<qlonglong>(m.id));
        it->setToolTip(qs(m.path) + "\n" + qs(m.videoCodec + " " + m.pixFmt + " " + m.audioCodec));
        if (m.hasVideo && QFileInfo::exists(qs(m.path))) {
            try {
                int tw = 96, th = std::max(2, static_cast<int>(96.0 * m.height / std::max(1, m.width)) / 2 * 2);
                Image8Ptr img = frames_.get(m, 0, tw, th);
                it->setIcon(QIcon(QPixmap::fromImage(QImage(img->px.data(), img->width, img->height, img->width * 4, QImage::Format_RGBA8888).copy())));
            } catch (...) {}
        } else if (!QFileInfo::exists(qs(m.path))) {
            it->setForeground(QColor("#FBBF24"));
            it->setText(it->text() + "  (missing)");
        }
        media_->addItem(it);
    }
}

// ---------------------------------------------------------------- editing
void MainWindow::splitAtPlayhead() {
    const Clip* sel = doc_.selected();
    Id target = kNoId;
    if (sel && sel->contains(doc_.playhead)) target = sel->id;
    else if (auto* c = doc_.comp())
        for (auto it = c->tracks.rbegin(); it != c->tracks.rend() && !target; ++it)
            if (!it->locked) if (const Clip* cl = it->clipAt(doc_.playhead)) target = cl->id;
    if (!target) { statusBar()->showMessage("No clip under the playhead", 3000); return; }
    doc_.tryEdit([&] { return edit::split(doc_.project, target, doc_.playhead); });
    doc_.endGesture();
}

void MainWindow::deleteSelected(bool ripple) {
    const Clip* sel = doc_.selected();
    if (!sel) return;
    Id id = sel->id;
    if (doc_.tryEdit([&] { return ripple ? edit::rippleDelete(doc_.project, id) : edit::deleteClip(doc_.project, id); })) doc_.select(kNoId);
    doc_.endGesture();
}

// ---------------------------------------------------------------- playback
void MainWindow::togglePlay() {
    if (playing_) { stopPlayback(); return; }
    auto* c = doc_.comp();
    if (!c) return;
    if (doc_.playhead >= c->durationFrames - 1) doc_.setPlayhead(0);
    playStartFrame_ = doc_.playhead;
    playing_ = true;
#ifdef KINETIC_HAS_AUDIO
    try {
        auto mix = exporter::mixAudio(doc_.project, *c, playStartFrame_, c->durationFrames, 48000, nullptr);
        QByteArray pcm;
        pcm.resize(static_cast<qsizetype>(mix.samples.size() * 2));
        auto* d = reinterpret_cast<int16_t*>(pcm.data());
        for (size_t i = 0; i < mix.samples.size(); ++i) d[i] = static_cast<int16_t>(std::lround(std::clamp(mix.samples[i], -1.f, 1.f) * 32767));
        audioBuf_ = new QBuffer(this);
        audioBuf_->setData(pcm);
        audioBuf_->open(QIODevice::ReadOnly);
        QAudioFormat fmt;
        fmt.setSampleRate(48000);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        sink_ = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
        sink_->start(audioBuf_);
    } catch (const std::exception& e) {
        statusBar()->showMessage(QString("Audio preview unavailable: ") + e.what(), 5000);
    }
#endif
    clock_.start();
    playTimer_.start(static_cast<int>(1000.0 / c->fps.toDouble()));
}

void MainWindow::stopPlayback() {
    playing_ = false;
    playTimer_.stop();
#ifdef KINETIC_HAS_AUDIO
    if (sink_) { sink_->stop(); sink_->deleteLater(); sink_ = nullptr; }
    if (audioBuf_) { audioBuf_->deleteLater(); audioBuf_ = nullptr; }
#endif
}

void MainWindow::playTick() {
    auto* c = doc_.comp();
    if (!c) return;
    double elapsed = clock_.nsecsElapsed() / 1e9;
#ifdef KINETIC_HAS_AUDIO
    if (sink_ && sink_->processedUSecs() > 0) elapsed = sink_->processedUSecs() / 1e6;  // audio is the master clock
#endif
    int64_t f = playStartFrame_ + static_cast<int64_t>(elapsed * c->fps.toDouble());
    if (f >= c->durationFrames) { stopPlayback(); doc_.setPlayhead(c->durationFrames - 1); return; }
    doc_.setPlayhead(f);  // the viewer renders this frame; slow frames are skipped, never slowed down
    timeline_->ensurePlayheadVisible();
}

// ---------------------------------------------------------------- beat sync
static std::vector<beatsync::BeatPoint> markerBeats(const Composition& c) {
    std::vector<beatsync::BeatPoint> out;
    for (auto& m : c.markers)
        if (m.kind == MarkerKind::Beat || m.kind == MarkerKind::StrongBeat) out.push_back({m.frame, m.strength, m.kind == MarkerKind::StrongBeat});
    return out;
}

void MainWindow::detectBeats() {
    auto* c = doc_.comp();
    const Clip* music = nullptr;
    for (auto& t : c->tracks) {
        if (t.kind != TrackKind::Audio || t.muted) continue;
        for (auto& cl : t.clips) if (!music) music = &cl;
    }
    if (!music) { QMessageBox::information(this, "Beat detection", "Put a music clip on an audio track first."); return; }
    const MediaRef* m = doc_.project.findMedia(music->mediaId);
    if (!m) { QMessageBox::warning(this, "Beat detection", "The music clip has no media."); return; }
    if (music->speed.animated() || std::fabs(music->speed.constant() - 1) > 1e-9)
        statusBar()->showMessage("Note: music clip is speed-changed; beats are mapped through its velocity curve", 6000);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    audio::BeatAnalysis a;
    try {
        auto buf = media::decodeAudio(m->path, 44100, 2);
        a = audio::analyzeBeats(buf.mono(), buf.sampleRate);
    } catch (const std::exception& e) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, "Beat detection", qs(e.what()));
        return;
    }
    QApplication::restoreOverrideCursor();
    // Map source-time beats to composition frames through the clip (sourceIn + velocity).
    auto toFrame = [&](double srcT, int64_t& out) {
        double fps = c->fps.toDouble();
        for (int64_t f = music->start; f < music->end(); ++f) {
            double s0 = Velocity::sourceTime(*music, (f - music->start) / fps), s1 = Velocity::sourceTime(*music, (f + 1 - music->start) / fps);
            if (srcT >= std::min(s0, s1) && srcT < std::max(s0, s1)) { out = f; return true; }
        }
        return false;
    };
    std::vector<beatsync::BeatPoint> beats;
    int strong = 0;
    for (auto& b : a.beats) { int64_t f; if (toFrame(b.time, f)) { beats.push_back({f, b.strength, b.strong}); strong += b.strong; } }
    QString drops;
    for (auto& e : a.events) if (e.second == "drop") drops += QString(" %1s").arg(e.first, 0, 'f', 2);
    auto plan = beatsync::planBeatMarkers(doc_.project, c->id, beats);
    QString text = QString("Tempo: %1 BPM\nBeats: %2 (%3 strong)\nOnsets: %4\nDrops:%5\n\nAdd %6 beat markers to the timeline?")
                       .arg(a.tempo, 0, 'f', 1).arg(beats.size()).arg(strong).arg(a.onsets.size())
                       .arg(drops.isEmpty() ? " none" : drops).arg(plan.changes);
    if (!plan.command || QMessageBox::question(this, "Detected beats", text) != QMessageBox::Yes) return;
    doc_.push(std::move(plan.command));
    // Drops become section markers.
    auto macro = std::make_unique<MacroCommand>("Add Drop Markers");
    Id next = doc_.project.nextId;
    for (auto& e : a.events) {
        int64_t f;
        if (e.second != "drop" || !toFrame(e.first, f)) continue;
        Marker mk;
        mk.id = next++;
        mk.frame = f;
        mk.kind = MarkerKind::Section;
        mk.color = "#F87171";
        mk.label = "drop";
        macro->add(std::make_unique<AddMarkerCmd>(c->id, mk));
    }
    if (!macro->empty()) {
        macro->add(std::make_unique<SetNextIdCmd>(doc_.project.nextId, next));
        doc_.push(std::move(macro));
    }
    doc_.endGesture();
}

void MainWindow::cutAtBeats(bool strongOnly) {
    const Clip* sel = doc_.selected();
    if (!sel) { statusBar()->showMessage("Select a clip first", 3000); return; }
    auto plan = beatsync::planCutsAtBeats(doc_.project, sel->id, markerBeats(*doc_.comp()), strongOnly);
    if (!plan.command) { QMessageBox::information(this, "Cut at beats", qs(plan.summary.front()) + "\n(Run Beat Sync › Detect Beats first.)"); return; }
    if (QMessageBox::question(this, "Review cuts", QString("Split the clip at %1 beats?").arg(plan.changes)) == QMessageBox::Yes) {
        doc_.push(std::move(plan.command));
        doc_.endGesture();
    }
}

void MainWindow::flashesOnStrongBeats() {
    auto* c = doc_.comp();
    Id track = kNoId;
    for (auto& t : c->tracks) if (t.kind == TrackKind::Video && t.name == "Flashes") track = t.id;
    if (!track) {
        Track t;
        t.id = doc_.project.nextId;
        t.kind = TrackKind::Video;
        t.name = "Flashes";
        t.color = "#FBBF24";
        size_t idx = 0;
        for (size_t i = 0; i < c->tracks.size(); ++i) if (c->tracks[i].kind == TrackKind::Video) idx = i + 1;
        auto m = std::make_unique<MacroCommand>("Add Flashes Track");
        m->add(std::make_unique<SetNextIdCmd>(doc_.project.nextId, doc_.project.nextId + 1));
        m->add(std::make_unique<AddTrackCmd>(c->id, t, idx));
        doc_.push(std::move(m));
        track = t.id;
    }
    int len = static_cast<int>(std::lround(c->fps.toDouble() * 0.12));
    auto plan = beatsync::planFlashesOnBeats(doc_.project, track, markerBeats(*doc_.comp()), std::max(2, len), {1, 1, 1, 1}, 0.6, true);
    if (!plan.command) { statusBar()->showMessage("No strong beat markers — run Detect Beats first", 4000); return; }
    if (QMessageBox::question(this, "Review flashes", qs(plan.summary.front()) + "?") == QMessageBox::Yes) doc_.push(std::move(plan.command));
    doc_.endGesture();
}

void MainWindow::shakeOnBeats() {
    const Clip* sel = doc_.selected();
    if (!sel) { statusBar()->showMessage("Select a clip first", 3000); return; }
    Clip c = *sel;
    if (c.shake.frequency <= 0) c.shake.frequency = 14;
    auto plan = beatsync::planShakeOnBeats(doc_.project, c.id, markerBeats(*doc_.comp()), 24, 0.18, false);
    if (plan.changes == 0) { statusBar()->showMessage("No beat markers inside the clip", 4000); return; }
    doc_.push(std::move(plan.command));
    doc_.endGesture();
}

// ---------------------------------------------------------------- export
void MainWindow::exportVideo() {
    auto* c = doc_.comp();
    QString out = QFileDialog::getSaveFileName(this, "Export", qs(doc_.project.settings.name) + ".mp4",
                                               "MP4 (*.mp4);;QuickTime (*.mov);;WebM (*.webm);;Matroska (*.mkv);;WAV audio (*.wav);;PNG sequence (*.png)");
    if (out.isEmpty()) return;
    bool ok = false;
    QString q = QInputDialog::getItem(this, "Export quality", "Quality (encoder effort/bitrate — effects are never downgraded):",
                                      {"draft", "balanced", "high", "maximum"}, 2, false, &ok);
    if (!ok) return;
    exporter::ExportSettings s;
    s.outputPath = ss(out);
    if (out.endsWith(".png", Qt::CaseInsensitive)) s.outputPath = ss(out.left(out.size() - 4)) + "_%05d.png";
    exporter::parseQuality(ss(q), s.quality);
    auto* dlg = new QProgressDialog("Exporting…", "Cancel", 0, static_cast<int>(c->durationFrames), this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(dlg, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    auto result = std::make_shared<QString>();
    Project snapshot = doc_.project;
    Id compId = c->id;
    QThread* th = QThread::create([snapshot, compId, s, cancel, result, dlg]() {
        media::FrameProvider fp(size_t(512) << 20);
        try {
            auto r = exporter::exportComposition(snapshot, compId, s, fp, [dlg](const exporter::Progress& p) {
                int done = static_cast<int>(p.done);
                QString label = QString("Exporting… %1 fps, about %2 s left").arg(p.framesPerSecond, 0, 'f', 1).arg(static_cast<int>(p.etaSeconds));
                QMetaObject::invokeMethod(dlg, [dlg, done, label] { dlg->setValue(done); dlg->setLabelText(label); }, Qt::QueuedConnection);
            }, cancel.get());
            QString msg = QString("Wrote %1\n%2 frames with %3 in %4 s").arg(qs(r.outputPath)).arg(r.frames).arg(qs(r.encoder)).arg(r.seconds, 0, 'f', 1);
            for (auto& w : r.warnings) msg += "\n⚠ " + qs(w);
            *result = msg;
        } catch (const std::exception& e) {
            *result = "Export failed: " + qs(e.what());
        }
    });
    connect(th, &QThread::finished, this, [this, th, dlg, result] {
        dlg->close();
        dlg->deleteLater();
        th->deleteLater();
        QMessageBox::information(this, "Export", *result);
    });
    th->start();
}

// ---------------------------------------------------------------- workspaces
void MainWindow::applyWorkspace(const QString& name) {
    QSettings st;
    if (!workspace_.isEmpty() && workspace_ != name) st.setValue("layout/" + workspace_, saveState());
    workspace_ = name;
    QByteArray saved = st.value("layout/" + name).toByteArray();
    if (!saved.isEmpty() && restoreState(saved)) { advanced_ = name == "Effects Studio"; rebuildInspector(); return; }
    bool vel = name == "Velocity Studio";
    mediaDock_->setVisible(name != "Velocity Studio");
    inspectorDock_->setVisible(true);
    timelineDock_->setVisible(true);
    velocityDock_->setVisible(vel);
    advanced_ = name == "Effects Studio";
    if (name == "Audio Studio") resizeDocks({timelineDock_}, {height() / 2}, Qt::Vertical);
    else resizeDocks({timelineDock_}, {height() / 3}, Qt::Vertical);
    if (name == "Effects Studio") resizeDocks({inspectorDock_}, {width() / 3}, Qt::Horizontal);
    if (vel) resizeDocks({velocityDock_}, {width() / 2}, Qt::Horizontal);
    rebuildInspector();
}

// ---------------------------------------------------------------- inspector
void MainWindow::rebuildInspector() {
    if (rebuildingInspector_) return;
    rebuildingInspector_ = true;
    auto* root = new QWidget;
    auto* v = new QVBoxLayout(root);
    const Clip* sel = doc_.selected();
    if (!sel) {
        v->addWidget(new QLabel("Select a clip on the timeline.\n\nDrag media from the Media panel onto a track,\n"
                                "press S to split at the playhead, B to detect beats."));
        v->addStretch();
        inspectorScroll_->setWidget(root);
        rebuildingInspector_ = false;
        return;
    }
    const Id id = sel->id;
    auto* comp = doc_.comp();
    auto modify = [this, id](const std::string& key, const std::function<void(Clip&)>& fn) {
        const Clip* cur = doc_.project.findClipConst(id);
        if (!cur) return;
        Clip c = *cur;
        fn(c);
        doc_.tryEdit([&] { return edit::setClip(doc_.project, c, "Inspector: " + key, "insp:" + std::to_string(id) + ":" + key); });
    };
    auto group = [&](const QString& title) { auto* g = new QGroupBox(title); auto* f = new QFormLayout(g); v->addWidget(g); return f; };
    auto spin = [&](QFormLayout* f, const QString& label, const AnimCurve& curve, double scale, double lo, double hi, double step,
                    const std::string& key, std::function<void(Clip&, double)> apply) {
        auto* s = new QDoubleSpinBox;
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(step < 0.1 ? 3 : step < 1 ? 2 : 1);
        s->setValue(curve.evaluate(0) * scale);
        s->setKeyboardTracking(false);
        if (curve.animated()) { s->setEnabled(false); s->setToolTip("Keyframed — edit with presets/graph"); s->setSuffix("  ◆"); }
        connect(s, &QDoubleSpinBox::valueChanged, this, [=](double val) { modify(key, [&](Clip& c) { apply(c, val / scale); }); });
        connect(s, &QDoubleSpinBox::editingFinished, this, [this] { doc_.endGesture(); });
        f->addRow(label, s);
        return s;
    };

    // Clip
    auto* fc = group("Clip");
    auto* name = new QLineEdit(qs(sel->name));
    connect(name, &QLineEdit::editingFinished, this, [=] { modify("name", [&](Clip& c) { c.name = ss(name->text()); }); doc_.endGesture(); });
    fc->addRow("Name", name);
    double fps = comp->fps.toDouble();
    fc->addRow("Type", new QLabel(qs(toString(sel->kind))));
    fc->addRow("Start", new QLabel(QString("%1 s (frame %2)").arg(sel->start / fps, 0, 'f', 3).arg(sel->start)));
    fc->addRow("Duration", new QLabel(QString("%1 s (%2 frames)").arg(sel->duration / fps, 0, 'f', 3).arg(sel->duration)));
    auto* enabled = new QCheckBox("Enabled");
    enabled->setChecked(sel->enabled);
    connect(enabled, &QCheckBox::toggled, this, [=](bool on) { modify("enabled", [&](Clip& c) { c.enabled = on; }); doc_.endGesture(); });
    fc->addRow(enabled);

    const Track* track = nullptr;
    for (auto& t : comp->tracks) for (auto& cl : t.clips) if (cl.id == id) track = &t;
    if (track && track->kind == TrackKind::Audio) {
        auto* fa = group("Audio");
        auto* g = new QDoubleSpinBox;
        g->setRange(-60, 24);
        g->setSuffix(" dB");
        g->setValue(sel->gainDb);
        connect(g, &QDoubleSpinBox::valueChanged, this, [=](double d) { modify("gain", [&](Clip& c) { c.gainDb = d; }); });
        fa->addRow("Gain", g);
    } else if (sel->kind != ClipKind::Adjustment) {
        // Transform
        const Transform& tr = sel->transform;
        auto* ft = group("Transform");
        spin(ft, "Position X", tr.posX, 1, -10000, 10000, 1, "posX", [](Clip& c, double x) { c.transform.posX.setConstant(x); });
        spin(ft, "Position Y", tr.posY, 1, -10000, 10000, 1, "posY", [](Clip& c, double x) { c.transform.posY.setConstant(x); });
        spin(ft, "Scale %", tr.scale, 100, 1, 2000, 1, "scale", [](Clip& c, double x) { c.transform.scale.setConstant(x); });
        spin(ft, "Rotation °", tr.rotation, 1, -3600, 3600, 1, "rot", [](Clip& c, double x) { c.transform.rotation.setConstant(x); });
        spin(ft, "Opacity %", tr.opacity, 100, 0, 100, 1, "opacity", [](Clip& c, double x) { c.transform.opacity.setConstant(x); });
        auto* blend = new QComboBox;
        for (int b = 0; b <= static_cast<int>(BlendMode::Darken); ++b) blend->addItem(qs(toString(static_cast<BlendMode>(b))));
        blend->setCurrentIndex(static_cast<int>(sel->blend));
        connect(blend, &QComboBox::currentIndexChanged, this, [=](int i) { modify("blend", [&](Clip& c) { c.blend = static_cast<BlendMode>(i); }); doc_.endGesture(); });
        ft->addRow("Blend", blend);
        auto* fit = new QComboBox;
        fit->addItems({"none", "fit", "fill"});
        fit->setCurrentIndex(static_cast<int>(tr.fit));
        connect(fit, &QComboBox::currentIndexChanged, this, [=](int i) { modify("fit", [&](Clip& c) { c.transform.fit = static_cast<FitMode>(i); }); doc_.endGesture(); });
        ft->addRow("Fit", fit);
        if (sel->kind != ClipKind::Solid) {
            auto* p = new QPushButton("Punch-in on clip start");
            connect(p, &QPushButton::clicked, this, [=] {
                modify("punch", [](Clip& c) {
                    c.transform.scale.clearKeys();
                    c.transform.scale.setKey(0, 1.15);
                    c.transform.scale.setKey(0.25, 1.0);
                    c.transform.scale.setSegmentEase(0, EasePreset::EaseOut);
                });
                doc_.endGesture();
            });
            auto* r = new QPushButton("Reset transform keys");
            connect(r, &QPushButton::clicked, this, [=] { modify("resetT", [](Clip& c) { c.transform = Transform{}; }); doc_.endGesture(); });
            ft->addRow(p);
            ft->addRow(r);
        }

        // Velocity
        if (sel->kind == ClipKind::Media || sel->kind == ClipKind::Composition) {
            auto* fv = group("Velocity");
            spin(fv, "Constant speed ×", sel->speed, 1, -8, 16, 0.05, "speed", [](Clip& c, double x) { c.speed.setConstant(x); });
            auto* preset = new QComboBox;
            for (auto pr : allVelocityPresets()) preset->addItem(velocityPresetName(pr));
            auto* intensity = new QDoubleSpinBox;
            intensity->setRange(0.2, 3);
            intensity->setSingleStep(0.1);
            intensity->setValue(1);
            auto* apply = new QPushButton("Apply preset");
            connect(apply, &QPushButton::clicked, this, [=] {
                VelocityPreset pr;
                velocityPresetFromName(ss(preset->currentText()), pr);
                double k = intensity->value();
                modify("velPreset", [&](Clip& c) { Velocity::applyPreset(c.speed, pr, c.duration / fps, k, 0.5); });
                doc_.endGesture();
            });
            auto* flat = new QPushButton("Reset to 1×");
            connect(flat, &QPushButton::clicked, this, [=] { modify("velReset", [](Clip& c) { c.speed = AnimCurve(1.0); }); doc_.endGesture(); });
            fv->addRow("Preset", preset);
            fv->addRow("Intensity", intensity);
            fv->addRow(apply);
            fv->addRow(flat);
            auto* interp = new QComboBox;
            interp->addItems({"Frame sampling (fast)", "Frame blending", "Optical flow (module not installed)"});
            interp->setCurrentIndex(static_cast<int>(sel->timeInterp));
            connect(interp, &QComboBox::currentIndexChanged, this, [=](int i) {
                if (i == 2) { statusBar()->showMessage("Optical flow is not installed; it would refuse to render rather than fake it.", 6000); interp->setCurrentIndex(1); return; }
                modify("interp", [&](Clip& c) { c.timeInterp = static_cast<TimeInterp>(i); });
                doc_.endGesture();
            });
            fv->addRow("Interpolation", interp);
            auto* open = new QPushButton("Open Velocity Studio");
            connect(open, &QPushButton::clicked, this, [this] { applyWorkspace("Velocity Studio"); });
            fv->addRow(open);
        }

        // Camera shake
        auto* fs = group("Camera Shake");
        auto* on = new QCheckBox("Enabled");
        on->setChecked(sel->shake.enabled);
        connect(on, &QCheckBox::toggled, this, [=](bool b) { modify("shakeOn", [&](Clip& c) { c.shake.enabled = b; if (b && !c.shake.amplitude.animated() && c.shake.amplitude.constant() == 0) c.shake.amplitude.setConstant(12); }); doc_.endGesture(); });
        fs->addRow(on);
        spin(fs, "Amplitude px", sel->shake.amplitude, 1, 0, 500, 1, "shakeAmp", [](Clip& c, double x) { c.shake.amplitude.setConstant(x); });
        auto plain = [&](const QString& label, double val, double lo, double hi, double step, const std::string& key, std::function<void(Clip&, double)> f) {
            auto* s = new QDoubleSpinBox;
            s->setRange(lo, hi);
            s->setSingleStep(step);
            s->setValue(val);
            s->setKeyboardTracking(false);
            connect(s, &QDoubleSpinBox::valueChanged, this, [=](double x) { modify(key, [&](Clip& c) { f(c, x); }); });
            connect(s, &QDoubleSpinBox::editingFinished, this, [this] { doc_.endGesture(); });
            fs->addRow(label, s);
        };
        plain("Frequency Hz", sel->shake.frequency, 0.1, 60, 0.5, "shakeFreq", [](Clip& c, double x) { c.shake.frequency = x; });
        plain("Rotation °", sel->shake.rotation, 0, 45, 0.5, "shakeRot", [](Clip& c, double x) { c.shake.rotation = x; });
        plain("Zoom jitter", sel->shake.zoom, 0, 0.5, 0.01, "shakeZoom", [](Clip& c, double x) { c.shake.zoom = x; });
        plain("Randomness", sel->shake.randomness, 0, 1, 0.05, "shakeRand", [](Clip& c, double x) { c.shake.randomness = x; });
        plain("Decay /s", sel->shake.decay, 0, 30, 0.5, "shakeDecay", [](Clip& c, double x) { c.shake.decay = x; });
        plain("Seed", static_cast<double>(sel->shake.seed), 0, 1e6, 1, "shakeSeed", [](Clip& c, double x) { c.shake.seed = static_cast<int64_t>(x); });
    }

    // Effects
    auto& reg = render::EffectRegistry::instance();
    auto* fxBox = new QGroupBox("Effects");
    auto* fxv = new QVBoxLayout(fxBox);
    v->addWidget(fxBox);
    auto* top = new QHBoxLayout;
    auto* bypass = new QCheckBox("Bypass stack");
    bypass->setChecked(sel->effectsBypassed);
    connect(bypass, &QCheckBox::toggled, this, [=](bool b) { modify("bypass", [&](Clip& c) { c.effectsBypassed = b; }); doc_.endGesture(); });
    auto* adv = new QCheckBox("Advanced");
    adv->setChecked(advanced_);
    connect(adv, &QCheckBox::toggled, this, [this](bool b) { advanced_ = b; QTimer::singleShot(0, this, &MainWindow::rebuildInspector); });
    top->addWidget(bypass);
    top->addWidget(adv);
    fxv->addLayout(top);
    for (size_t i = 0; i < sel->effects.size(); ++i) {
        const EffectInstance& e = sel->effects[i];
        const render::EffectDef* def = reg.find(e.defId, e.defVersion);
        auto* g = new QGroupBox(def ? qs(def->name) : qs(e.defId) + " (unavailable — passes through)");
        auto* f = new QFormLayout(g);
        auto* row = new QHBoxLayout;
        auto* en = new QCheckBox("On");
        en->setChecked(e.enabled);
        connect(en, &QCheckBox::toggled, this, [=](bool b) { modify("fxOn" + std::to_string(i), [&](Clip& c) { c.effects[i].enabled = b; }); doc_.endGesture(); });
        row->addWidget(en);
        auto mk = [&](const char* t, std::function<void(Clip&)> fn) {
            auto* b = new QPushButton(t);
            b->setMaximumWidth(34);
            connect(b, &QPushButton::clicked, this, [=] { modify("fxOp", fn); doc_.endGesture(); });
            row->addWidget(b);
        };
        if (i > 0) mk("▲", [i](Clip& c) { std::swap(c.effects[i], c.effects[i - 1]); });
        if (i + 1 < sel->effects.size()) mk("▼", [i](Clip& c) { std::swap(c.effects[i], c.effects[i + 1]); });
        mk("⧉", [i](Clip& c) { c.effects.insert(c.effects.begin() + static_cast<long>(i) + 1, c.effects[i]); });
        mk("✕", [i](Clip& c) { c.effects.erase(c.effects.begin() + static_cast<long>(i)); });
        f->addRow(row);
        auto* mix = new QDoubleSpinBox;
        mix->setRange(0, 100);
        mix->setSuffix(" %");
        mix->setValue(e.mix * 100);
        connect(mix, &QDoubleSpinBox::valueChanged, this, [=](double x) { modify("fxMix" + std::to_string(i), [&](Clip& c) { c.effects[i].mix = x / 100; }); });
        f->addRow("Intensity", mix);
        if (def)
            for (auto& pd : def->params) {
                if (!advanced_ && !pd.quick) continue;
                AnimCurve cur = e.param(pd.id) ? *e.param(pd.id) : AnimCurve(pd.def);
                double step = (pd.max - pd.min) > 50 ? 1 : (pd.max - pd.min) > 5 ? 0.1 : 0.01;
                std::string pid = pd.id;
                spin(f, qs(pd.name), cur, 1, pd.min, pd.max, step, "fx" + std::to_string(i) + pid,
                     [i, pid](Clip& c, double x) { c.effects[i].paramOrAdd(pid, x).setConstant(x); });
            }
        fxv->addWidget(g);
    }
    auto* addRow = new QHBoxLayout;
    auto* pick = new QComboBox;
    for (auto* d : reg.list()) pick->addItem(qs(d->category + " › " + d->name), qs(d->id));
    auto* add = new QPushButton("Add");
    connect(add, &QPushButton::clicked, this, [=, &reg] {
        std::string fx = ss(pick->currentData().toString());
        modify("fxAdd", [&](Clip& c) { c.effects.push_back(reg.instantiate(fx)); });
        doc_.endGesture();
    });
    addRow->addWidget(pick, 1);
    addRow->addWidget(add);
    fxv->addLayout(addRow);
    v->addStretch();
    inspectorScroll_->setWidget(root);
    rebuildingInspector_ = false;
}
