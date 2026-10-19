// SPDX-License-Identifier: GPL-3.0-or-later
#include "clipboardmanager.h"

#include "waylandclipboard.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QImageReader>
#include <QQuickImageProvider>
#include <QTimer>
#include <QUrl>

namespace {

QString localPath(const QString &path)
{
    if (!path.startsWith(QLatin1String("file:")))
        return path;
    const QUrl url(path);
    return url.isLocalFile() ? url.toLocalFile() : path;
}

} // namespace

// ============================================================================
// ClipboardImageProvider
// ============================================================================

/**
 * Serves `image://clipboard/<uid>` URLs from the in-memory history entries,
 * so QML can keep using `Image { source: model.imagePath }` without any file
 * on disk.
 */
class ClipboardImageProvider : public QQuickImageProvider
{
public:
    explicit ClipboardImageProvider(ClipboardHistoryModel *model)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_model(model)
    {
    }

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override
    {
        bool ok = false;
        const int uid = id.toInt(&ok);

        QImage image;
        if (ok) {
            if (const ClipboardHistoryModel::Entry *entry = m_model->findByUid(uid)) {
                if (!entry->imageData.isEmpty())
                    image.loadFromData(entry->imageData,
                                       entry->mime.toLatin1().constData());
            }
        }

        if (size)
            *size = image.size();
        if (!image.isNull() && !requestedSize.isEmpty())
            image = image.scaled(requestedSize, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        return image;
    }

private:
    ClipboardHistoryModel *m_model;
};

// ============================================================================
// ClipboardHistoryModel
// ============================================================================

ClipboardHistoryModel::ClipboardHistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ClipboardHistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant ClipboardHistoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case UidRole:
        return entry.uid;
    case KindRole:
        return entry.kind;
    case ClipTextRole:
        return entry.clipText;
    case ImagePathRole:
        return entry.imagePath;
    case MimeRole:
        return entry.mime;
    case ImageHashRole:
        return entry.imageHash;
    case BytesRole:
        return entry.bytes;
    case ImgWRole:
        return entry.imgW;
    case ImgHRole:
        return entry.imgH;
    case TimeRole:
        return entry.time;
    default:
        return {};
    }
}

QHash<int, QByteArray> ClipboardHistoryModel::roleNames() const
{
    return {
        {UidRole, "uid"},
        {KindRole, "kind"},
        {ClipTextRole, "clipText"},
        {ImagePathRole, "imagePath"},
        {MimeRole, "mime"},
        {ImageHashRole, "imageHash"},
        {BytesRole, "bytes"},
        {ImgWRole, "imgW"},
        {ImgHRole, "imgH"},
        {TimeRole, "time"},
    };
}

const ClipboardHistoryModel::Entry *ClipboardHistoryModel::findByUid(int uid) const
{
    for (const Entry &entry : m_entries) {
        if (entry.uid == uid)
            return &entry;
    }
    return nullptr;
}

void ClipboardHistoryModel::append(const Entry &entry)
{
    const int row = m_entries.size();
    beginInsertRows(QModelIndex(), row, row);
    m_entries.append(entry);
    endInsertRows();
}

void ClipboardHistoryModel::insertAt(int row, const Entry &entry)
{
    row = qBound(0, row, m_entries.size());
    beginInsertRows(QModelIndex(), row, row);
    m_entries.insert(row, entry);
    endInsertRows();
}

void ClipboardHistoryModel::removeAt(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_entries.removeAt(row);
    endRemoveRows();
}

void ClipboardHistoryModel::clear()
{
    if (m_entries.isEmpty())
        return;
    beginResetModel();
    m_entries.clear();
    endResetModel();
}

// ============================================================================
// ClipboardManager
// ============================================================================

ClipboardManager::ClipboardManager(QObject *parent)
    : QObject(parent)
    , m_history(new ClipboardHistoryModel(this))
{
    // Data-control client: observes and replaces the selection without
    // needing keyboard focus, unlike QClipboard.
    m_wayland = new WaylandClipboard(this);
    connect(m_wayland, &WaylandClipboard::textReceived,
            this, &ClipboardManager::addText);
    connect(m_wayland, &WaylandClipboard::imageReceived, this,
            [this](const QByteArray &data, const QString &mime) {
                addImageData(data, mime);
            });
    m_wayland->start();

    // componentComplete() is the normal registration path; this is a fallback
    // for engines that do not call QQmlParserStatus on singleton instances.
    QTimer::singleShot(0, this, &ClipboardManager::ensureImageProvider);
}

ClipboardManager::~ClipboardManager() = default;

void ClipboardManager::componentComplete()
{
    ensureImageProvider();
}

void ClipboardManager::ensureImageProvider()
{
    if (m_imageProviderRegistered)
        return;

    QQmlEngine *engine = qmlEngine(this);
    if (!engine)
        return;

    // QQmlEngine takes ownership of the provider.
    engine->addImageProvider(QStringLiteral("clipboard"),
                             new ClipboardImageProvider(m_history));
    m_imageProviderRegistered = true;
}

QAbstractListModel *ClipboardManager::history() const
{
    return m_history;
}

int ClipboardManager::count() const
{
    return m_history->rowCount();
}

void ClipboardManager::setMaxHistory(int maxHistory)
{
    maxHistory = qMax(1, maxHistory);
    if (m_maxHistory == maxHistory)
        return;
    m_maxHistory = maxHistory;
    emit maxHistoryChanged();
    trimHistory();
    emit countChanged();
}

int ClipboardManager::findDuplicate(const QString &kind, const QString &text,
                                    const QString &hash) const
{
    const QVector<ClipboardHistoryModel::Entry> &entries = m_history->entries();
    for (int i = 0; i < entries.size(); ++i) {
        const ClipboardHistoryModel::Entry &entry = entries.at(i);
        if (entry.kind != kind)
            continue;
        if (kind == QLatin1String("text")) {
            if (entry.clipText == text)
                return i;
        } else if (!hash.isEmpty() && entry.imageHash == hash) {
            return i;
        }
    }
    return -1;
}

void ClipboardManager::addText(const QString &raw)
{
    QString text = raw;
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
        text.chop(1);
    if (text.trimmed().isEmpty())
        return;

    const int existing = findDuplicate(QStringLiteral("text"), text, {});
    if (existing == 0) {
        // Copying from our own history must not create a second entry.
        setCurrent(QStringLiteral("text"), text);
        return;
    }

    ClipboardHistoryModel::Entry entry;
    if (existing > 0) {
        // Promote the existing entry instead of duplicating it and keep its
        // uid stable for delegates that still reference it.
        entry = m_history->at(existing);
        m_history->removeAt(existing);
    } else {
        entry.uid = m_nextUid++;
        entry.kind = QStringLiteral("text");
        entry.clipText = text;
    }
    entry.time = QDateTime::currentMSecsSinceEpoch();

    m_history->insertAt(0, entry);
    trimHistory();
    emit countChanged();
    syncCurrent();
    emit historyUpdated();
}

void ClipboardManager::addImageData(const QByteArray &data, const QString &mime)
{
    if (data.isEmpty())
        return;

    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

    const int existing = findDuplicate(QStringLiteral("image"), {}, hash);
    if (existing == 0) {
        // Same image copied again: nothing to store.
        setCurrent(QStringLiteral("image"), QString());
        return;
    }

    ClipboardHistoryModel::Entry entry;
    if (existing > 0) {
        // Re-use the entry (and its bytes) and move it to the top.
        entry = m_history->at(existing);
        m_history->removeAt(existing);
    } else {
        entry.uid = m_nextUid++;
        entry.kind = QStringLiteral("image");
        entry.mime = mime;
        entry.imageHash = hash;
        entry.imageData = data;
        entry.bytes = data.size();
        entry.imagePath = QStringLiteral("image://clipboard/%1").arg(entry.uid);

        // Header-only read: no need to decode the whole image just for the
        // dimensions; the image provider decodes it lazily when QML displays it.
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, mime.toLatin1().constData());
        const QSize size = reader.size();
        if (size.isValid()) {
            entry.imgW = size.width();
            entry.imgH = size.height();
        }
    }
    entry.time = QDateTime::currentMSecsSinceEpoch();

    m_history->insertAt(0, entry);
    trimHistory();
    emit countChanged();
    syncCurrent();
    emit historyUpdated();
}

void ClipboardManager::trimHistory()
{
    while (m_history->rowCount() > m_maxHistory)
        dropRow(m_history->rowCount() - 1);
}

void ClipboardManager::dropRow(int row)
{
    if (row < 0 || row >= m_history->rowCount())
        return;
    m_history->removeAt(row);
}

void ClipboardManager::removeEntry(int uid)
{
    const QVector<ClipboardHistoryModel::Entry> &entries = m_history->entries();
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).uid == uid) {
            dropRow(i);
            emit countChanged();
            syncCurrent();
            emit historyUpdated();
            return;
        }
    }
}

void ClipboardManager::clearHistory()
{
    m_history->clear();
    setCurrent(QStringLiteral("text"), QString());
    emit countChanged();
    emit historyUpdated();
}

void ClipboardManager::syncCurrent()
{
    if (m_history->rowCount() > 0) {
        const ClipboardHistoryModel::Entry &entry = m_history->at(0);
        setCurrent(entry.kind, entry.kind == QLatin1String("text") ? entry.clipText : QString());
    } else {
        setCurrent(QStringLiteral("text"), QString());
    }
}

void ClipboardManager::setCurrent(const QString &kind, const QString &text)
{
    if (m_currentKind == kind && m_currentText == text)
        return;
    m_currentKind = kind;
    m_currentText = text;
    emit currentChanged();
}

void ClipboardManager::copyToClipboard(const QString &text)
{
    if (text.isEmpty())
        return;
    m_wayland->setText(text);
    // The compositor echoes the new selection back through the data-control
    // device; reflect it immediately for the status bar.
    setCurrent(QStringLiteral("text"), text);
}

void ClipboardManager::copyImage(const QString &path, const QString &mime)
{
    QByteArray data;
    QString type = mime;

    if (path.startsWith(QLatin1String("image://clipboard/"))) {
        bool ok = false;
        static const QString prefix = QStringLiteral("image://clipboard/");
        const int uid = path.mid(prefix.size()).toInt(&ok);
        const ClipboardHistoryModel::Entry *entry = ok ? m_history->findByUid(uid) : nullptr;
        if (!entry || entry->imageData.isEmpty()) {
            qWarning() << "[ClipboardManager] unknown clipboard image:" << path;
            return;
        }
        data = entry->imageData;
        if (type.isEmpty())
            type = entry->mime;
    } else {
        QFile file(localPath(path));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[ClipboardManager] cannot open image:" << path;
            return;
        }
        data = file.readAll();
        if (type.isEmpty())
            type = QStringLiteral("image/png");
    }

    m_wayland->setImage(data, type);
    setCurrent(QStringLiteral("image"), QString());
}
