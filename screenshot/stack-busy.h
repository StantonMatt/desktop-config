#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <QPointer>
#include <QSharedPointer>

class QScreen;
class QWidget;
class QTemporaryFile;

// Separate from the capture overlays: this surface must never accept input or focus.
class StackBusy final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.local.QuickRegionOverlay.StackBusy")
public:
    explicit StackBusy(QObject *parent = nullptr);
    ~StackBusy() override;
    void setBusy(bool busy);
public slots:
    void CursorPosition(const QString &token, int x, int y);
private:
    QTimer delay, animation;
    QElapsedTimer rotation;
    QHash<QScreen *, QWidget *> windows;
    QPointer<QWidget> visible;
    QString token, scriptName;
    QSharedPointer<QTemporaryFile> trackerFile;
    bool active = false;
    void startTracking();
    void unload(const QString &name);
};
