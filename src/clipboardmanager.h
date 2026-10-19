// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QObject>
#include <QQmlEngine>
#include <QQmlParserStatus>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class WaylandClipboard;

/**
 * History backing store for ClipboardManager.
 *
 * The role names match the QML delegate's `required property` names used by
 * the clipboard popup (uid, kind, clipText, imagePath, mime, imageHash,
 * bytes, imgW, imgH, time), so an instance can be passed directly to a
 * Repeater/ListView as its model.
 *
 * Image data is kept in memory only.  `imagePath` is therefore not a file
 * path but a `image://clipboard/<uid>` URL served by ClipboardImageProvider,
 * which works unchanged with `Image { source: model.imagePath }`.
 */
class ClipboardHistoryModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    enum Role {
        UidRole = Qt::UserRole + 1,
        KindRole,
        ClipTextRole,
        ImagePathRole,
        MimeRole,
        ImageHashRole,
        BytesRole,
        ImgWRole,
        ImgHRole,
        TimeRole,
    };

    struct Entry {
        int uid = 0;
        QString kind; // "text" or "image"
        QString clipText;
        QString imagePath; // "image://clipboard/<uid>" for images
        QString mime;
        QString imageHash;
        QByteArray imageData; // encoded image (PNG/JPEG/...), memory only
        qint64 bytes = 0;
        int imgW = 0;
        int imgH = 0;
        qint64 time = 0;
    };

    explicit ClipboardHistoryModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isEmpty() const { return m_entries.isEmpty(); }
    const Entry &at(int row) const { return m_entries.at(row); }
    const QVector<Entry> &entries() const { return m_entries; }
    const Entry *findByUid(int uid) const;

    void append(const Entry &entry);
    void insertAt(int row, const Entry &entry);
    void removeAt(int row);
    void clear();

private:
    QVector<Entry> m_entries;
};

/**
 * QML singleton that owns the Wayland clipboard history.
 *
 * Wayland deliberately does not give ordinary clients a global clipboard
 * feed: QClipboard (and therefore Quickshell.clipboardText) only observes
 * changes while the application holds keyboard focus.  The clipboard-manager
 * role therefore needs the data-control protocol.  WaylandClipboard speaks
 * ext-data-control-v1 (KWin, niri, ...) and wlr-data-control-unstable-v1
 * (wlroots: Hyprland, Sway, ...) directly, without any helper program.
 *
 * The manager is a pure runtime service: it captures the selection while the
 * process is alive, keeps the history (including image bytes) in memory, and
 * releases the selection when the process exits.  Nothing is written to disk.
 */
class ClipboardManager : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractListModel *history READ history CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString currentText READ currentText NOTIFY currentChanged)
    Q_PROPERTY(QString currentKind READ currentKind NOTIFY currentChanged)
    Q_PROPERTY(int maxHistory READ maxHistory WRITE setMaxHistory NOTIFY maxHistoryChanged)
    /// True when large images can be published without blocking the shell.
    /// Older builds served the selection with a blocking write, which froze
    /// the event loop for payloads bigger than the pipe buffer (screenshots).
    Q_PROPERTY(bool asyncSend READ asyncSend CONSTANT)

public:
    explicit ClipboardManager(QObject *parent = nullptr);
    ~ClipboardManager() override;

    // QQmlParserStatus: used to register the image provider with the engine
    // that owns the singleton.
    void classBegin() override { }
    void componentComplete() override;

    QAbstractListModel *history() const;
    int count() const;
    QString currentText() const { return m_currentText; }
    QString currentKind() const { return m_currentKind; }
    int maxHistory() const { return m_maxHistory; }
    void setMaxHistory(int maxHistory);

    bool asyncSend() const;

    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE void copyImage(const QString &path, const QString &mime = QString());
    Q_INVOKABLE void removeEntry(int uid);
    Q_INVOKABLE void clearHistory();

signals:
    void countChanged();
    void currentChanged();
    void maxHistoryChanged();
    void historyUpdated();

private:
    void ensureImageProvider();

    void addText(const QString &raw);
    void addImageData(const QByteArray &data, const QString &mime);

    int findDuplicate(const QString &kind, const QString &text, const QString &hash) const;
    void insertEntry(const ClipboardHistoryModel::Entry &entry);
    void trimHistory();
    void dropRow(int row);
    void syncCurrent();
    void setCurrent(const QString &kind, const QString &text);

    ClipboardHistoryModel *m_history = nullptr;
    WaylandClipboard *m_wayland = nullptr;
    bool m_imageProviderRegistered = false;

    QString m_currentText;
    QString m_currentKind = QStringLiteral("text");
    int m_maxHistory = 50;
    int m_nextUid = 1;
};
