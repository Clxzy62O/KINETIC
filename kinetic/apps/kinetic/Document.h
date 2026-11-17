#pragma once
// The open project + its undo stack. All UI mutation goes through push() so undo always works.
#include <QObject>

#include <algorithm>
#include <QString>

#include "edit/Commands.h"
#include "project/Model.h"

class Document : public QObject {
    Q_OBJECT
public:
    kinetic::Project project;
    kinetic::CommandStack stack;
    QString path;  // empty until first save
    kinetic::Id selectedClip = kinetic::kNoId;
    int64_t playhead = 0;

    kinetic::Composition* comp() { return project.findComposition(project.rootCompositionId); }
    const kinetic::Clip* selected() const { return project.findClipConst(selectedClip); }

    // Applies a command; on failure reports the reason and leaves the project unchanged.
    bool push(kinetic::CommandPtr cmd) {
        if (!cmd) return false;
        try {
            stack.push(project, std::move(cmd));
        } catch (const std::exception& e) {
            emit message(QString::fromStdString(e.what()));
            return false;
        }
        emit changed();
        return true;
    }
    template <class F> bool tryEdit(F&& makeCommand) {
        try {
            return push(makeCommand());
        } catch (const std::exception& e) {
            emit message(QString::fromStdString(e.what()));
            return false;
        }
    }
    void endGesture() { stack.endGesture(); }
    void undo() { if (stack.canUndo()) { stack.undo(project); fixSelection(); emit changed(); emit selectionChanged(); } }
    void redo() { if (stack.canRedo()) { stack.redo(project); fixSelection(); emit changed(); emit selectionChanged(); } }

    void select(kinetic::Id id) {
        if (id == selectedClip) return;
        selectedClip = id;
        emit selectionChanged();
    }
    void setPlayhead(int64_t f) {
        auto* c = comp();
        if (c) f = std::clamp<int64_t>(f, 0, std::max<int64_t>(0, c->durationFrames - 1));
        if (f == playhead) return;
        playhead = f;
        emit playheadChanged(f);
    }
    void replaceProject(kinetic::Project p, const QString& newPath) {
        project = std::move(p);
        stack.clear();
        path = newPath;
        selectedClip = kinetic::kNoId;
        playhead = 0;
        emit changed();
        emit selectionChanged();
        emit playheadChanged(0);
    }

signals:
    void changed();
    void selectionChanged();
    void playheadChanged(qint64 frame);
    void message(const QString& text);

private:
    void fixSelection() { if (!project.findClipConst(selectedClip)) selectedClip = kinetic::kNoId; }
};
