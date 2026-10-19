// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

/**
 * Clipboard manager client for the two Wayland "data control" protocols:
 *
 *   - ext-data-control-v1            (KWin, niri, ...)
 *   - wlr-data-control-unstable-v1   (wlroots: Hyprland, Sway, ...)
 *
 * These are the protocols that let an *unfocused* client observe and replace
 * the selection, which is exactly what a clipboard manager needs.  Ordinary
 * wl_data_device (and therefore QClipboard) only delivers selection events to
 * the client that currently holds keyboard focus.
 *
 * The class opens its own wl_display connection, so it works from any Qt
 * application and does not depend on QtWaylandClient or on the connection
 * QGuiApplication uses.  All events are integrated into the Qt event loop
 * through QSocketNotifier; no thread or child process is involved.
 */
class WaylandClipboard : public QObject
{
    Q_OBJECT

public:
    explicit WaylandClipboard(QObject *parent = nullptr);
    ~WaylandClipboard() override;

    /// Connect to the compositor.  Safe to call repeatedly.
    bool start();

    bool isAvailable() const;

    /// True when serving the selection is asynchronous, i.e. large payloads
    /// (images) can be published without blocking the event loop.
    bool asyncSend() const;
    /// "ext-data-control-v1", "wlr-data-control-unstable-v1" or empty.
    QString backendName() const;

    /// Take ownership of the selection and serve @p text to pasting clients.
    void setText(const QString &text);

    /// Take ownership of the selection and serve @p data as @p mime.
    void setImage(const QByteArray &data, const QString &mime);

    // Public only so the C protocol listener shims in the .cpp can name it.
    struct Impl;

signals:
    /// The selection changed to plain text (already decoded as UTF-8).
    void textReceived(const QString &text);

    /// The selection changed to image data of @p mime.
    void imageReceived(const QByteArray &data, const QString &mime);

    /// The selection was cleared (or holds no usable MIME type).
    void selectionUnset();

    void started();
    void failed(const QString &reason);

private:
    std::unique_ptr<Impl> d;
};
