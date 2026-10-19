// SPDX-License-Identifier: GPL-3.0-or-later
#include "waylandclipboard.h"

#include <QDebug>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <wayland-client.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "wayland-ext-data-control-v1-client-protocol.h"
#include "wayland-wlr-data-control-unstable-v1-client-protocol.h"

namespace {

// A clipboard entry larger than this is treated as a broken transfer.  The
// largest realistic case is a 4K screenshot (~30 MB uncompressed PNG).
constexpr int kMaxTransferBytes = 64 * 1024 * 1024;

const QList<QByteArray> &textMimeCandidates()
{
    static const QList<QByteArray> mimes = {
        QByteArrayLiteral("text/plain;charset=utf-8"),
        QByteArrayLiteral("text/plain;charset=UTF-8"),
        QByteArrayLiteral("UTF8_STRING"),
        QByteArrayLiteral("text/plain"),
        QByteArrayLiteral("TEXT"),
        QByteArrayLiteral("STRING"),
        QByteArrayLiteral("text/uri-list"),
    };
    return mimes;
}

const QList<QByteArray> &imageMimeCandidates()
{
    static const QList<QByteArray> mimes = {
        QByteArrayLiteral("image/png"),
        QByteArrayLiteral("image/jpeg"),
        QByteArrayLiteral("image/webp"),
        QByteArrayLiteral("image/gif"),
        QByteArrayLiteral("image/bmp"),
        QByteArrayLiteral("image/tiff"),
    };
    return mimes;
}

QString socketError()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct WaylandClipboard::Impl
{
    explicit Impl(WaylandClipboard *owner) : q(owner) { }
    ~Impl() { teardown(); }

    struct Source {
        Impl *impl = nullptr;
        wl_proxy *proxy = nullptr;
        bool ext = false;
        QByteArray data;
    };

    struct Offer {
        Impl *impl = nullptr;
        wl_proxy *proxy = nullptr;
        bool ext = false;
        QList<QByteArray> mimes;
    };

    WaylandClipboard *q = nullptr;

    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_seat *seat = nullptr;
    wl_proxy *manager = nullptr;
    wl_proxy *device = nullptr;

    bool ext = false;
    QString backend;

    uint32_t extName = 0;
    uint32_t extVersion = 1;
    uint32_t wlrName = 0;
    uint32_t wlrVersion = 2;
    uint32_t seatName = 0;
    uint32_t seatVersion = 1;

    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;

    Offer *pendingOffer = nullptr;
    Offer *selection = nullptr;
    QHash<wl_proxy *, Source *> sources;

    // One in-flight "serve the selection" transfer per receiving client.
    // The payload (a screenshot easily is a few megabytes) must not be
    // written in a single blocking call: when the receiving client is
    // ourselves -- the manager reads back the selection it just installed
    // to put it into the history -- the read side can only run in the event
    // loop we are blocking, so both ends wait for each other forever.
    struct Send {
        QByteArray data;
        int fd = -1;
        qsizetype offset = 0;
        QSocketNotifier *notifier = nullptr;
    };
    QSet<Send *> sends;

    // Incoming transfer (selection -> us).
    int transferFd = -1;
    QSocketNotifier *transferNotifier = nullptr;
    QByteArray transferData;
    QByteArray transferMime;
    int transferKind = 0; // 0 = none, 1 = text, 2 = image

    bool retryScheduled = false;
    bool warnedNoProtocol = false;
    // While the first round trip is running only collect global names, so the
    // manager choice does not depend on the order in which the compositor
    // happens to advertise them.
    bool collectingGlobals = true;

    // -- lifetime ----------------------------------------------------------
    bool connectDisplay();
    void teardown();
    void scheduleRetry(int delayMs = 5000);
    void handleDisconnect(const QString &reason);

    // -- registry ----------------------------------------------------------
    void handleGlobal(uint32_t name, const char *interface, uint32_t version);
    void handleGlobalRemove(uint32_t name);
    void ensureObjects();

    // -- dispatch ----------------------------------------------------------
    void installNotifiers();
    void flush();
    void onReadable();
    void onWritable();

    // -- device events -----------------------------------------------------
    void handleDataOffer(wl_proxy *proxy);
    void handleSelection(wl_proxy *proxy);
    void handlePrimarySelection(wl_proxy *proxy);
    void handleDeviceFinished();

    // -- offers ------------------------------------------------------------
    void addMime(Offer *offer, const char *mime);
    Offer *takePendingOffer(wl_proxy *proxy);
    void chooseOffer(Offer *offer);
    void requestData(Offer *offer, const QByteArray &mime, int kind);
    void onTransferReadable();
    void finishTransfer();
    void cancelTransfer();
    void destroyOffer(Offer *offer);

    // -- sources (we own the selection) ------------------------------------
    void installSource(const QByteArray &data, const QList<QByteArray> &mimes);
    void handleSourceSend(Source *source, const char *mime, int fd);
    void onSendWritable(Send *send);
    void finishSend(Send *send);
    void cancelSends();
    void handleSourceCancelled(Source *source);
    void destroySource(Source *source);
};

// -- registry ----------------------------------------------------------------

static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version)
{
    Q_UNUSED(registry)
    static_cast<WaylandClipboard::Impl *>(data)->handleGlobal(name, interface, version);
}

static void registryGlobalRemove(void *data, wl_registry *registry, uint32_t name)
{
    Q_UNUSED(registry)
    static_cast<WaylandClipboard::Impl *>(data)->handleGlobalRemove(name);
}

static const struct wl_registry_listener s_registryListener = {
    registryGlobal,
    registryGlobalRemove,
};

// -- ext device --------------------------------------------------------------

static void extDeviceDataOffer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handleDataOffer(reinterpret_cast<wl_proxy *>(id));
}

static void extDeviceSelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handleSelection(reinterpret_cast<wl_proxy *>(id));
}

static void extDeviceFinished(void *data, ext_data_control_device_v1 *)
{
    static_cast<WaylandClipboard::Impl *>(data)->handleDeviceFinished();
}

static void extDevicePrimarySelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handlePrimarySelection(reinterpret_cast<wl_proxy *>(id));
}

static const struct ext_data_control_device_v1_listener s_extDeviceListener = {
    extDeviceDataOffer,
    extDeviceSelection,
    extDeviceFinished,
    extDevicePrimarySelection,
};

// -- wlr device --------------------------------------------------------------

static void wlrDeviceDataOffer(void *data, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handleDataOffer(reinterpret_cast<wl_proxy *>(id));
}

static void wlrDeviceSelection(void *data, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handleSelection(reinterpret_cast<wl_proxy *>(id));
}

static void wlrDeviceFinished(void *data, zwlr_data_control_device_v1 *)
{
    static_cast<WaylandClipboard::Impl *>(data)->handleDeviceFinished();
}

static void wlrDevicePrimarySelection(void *data, zwlr_data_control_device_v1 *, zwlr_data_control_offer_v1 *id)
{
    static_cast<WaylandClipboard::Impl *>(data)
        ->handlePrimarySelection(reinterpret_cast<wl_proxy *>(id));
}

static const struct zwlr_data_control_device_v1_listener s_wlrDeviceListener = {
    wlrDeviceDataOffer,
    wlrDeviceSelection,
    wlrDeviceFinished,
    wlrDevicePrimarySelection,
};

// -- ext offer ---------------------------------------------------------------

static void extOfferOffer(void *data, ext_data_control_offer_v1 *, const char *mime)
{
    static_cast<WaylandClipboard::Impl::Offer *>(data)->impl->addMime(
        static_cast<WaylandClipboard::Impl::Offer *>(data), mime);
}

static const struct ext_data_control_offer_v1_listener s_extOfferListener = {
    extOfferOffer,
};

// -- wlr offer ---------------------------------------------------------------

static void wlrOfferOffer(void *data, zwlr_data_control_offer_v1 *, const char *mime)
{
    static_cast<WaylandClipboard::Impl::Offer *>(data)->impl->addMime(
        static_cast<WaylandClipboard::Impl::Offer *>(data), mime);
}

static const struct zwlr_data_control_offer_v1_listener s_wlrOfferListener = {
    wlrOfferOffer,
};

// -- ext source --------------------------------------------------------------

static void extSourceSend(void *data, ext_data_control_source_v1 *, const char *mime, int32_t fd)
{
    auto *source = static_cast<WaylandClipboard::Impl::Source *>(data);
    source->impl->handleSourceSend(source, mime, fd);
}

static void extSourceCancelled(void *data, ext_data_control_source_v1 *)
{
    auto *source = static_cast<WaylandClipboard::Impl::Source *>(data);
    source->impl->handleSourceCancelled(source);
}

static const struct ext_data_control_source_v1_listener s_extSourceListener = {
    extSourceSend,
    extSourceCancelled,
};

// -- wlr source --------------------------------------------------------------

static void wlrSourceSend(void *data, zwlr_data_control_source_v1 *, const char *mime, int32_t fd)
{
    auto *source = static_cast<WaylandClipboard::Impl::Source *>(data);
    source->impl->handleSourceSend(source, mime, fd);
}

static void wlrSourceCancelled(void *data, zwlr_data_control_source_v1 *)
{
    auto *source = static_cast<WaylandClipboard::Impl::Source *>(data);
    source->impl->handleSourceCancelled(source);
}

static const struct zwlr_data_control_source_v1_listener s_wlrSourceListener = {
    wlrSourceSend,
    wlrSourceCancelled,
};

// ============================================================================
// Impl: connection
// ============================================================================

bool WaylandClipboard::Impl::connectDisplay()
{
    if (display)
        return true;

    display = wl_display_connect(nullptr);
    if (!display) {
        const QString reason = QStringLiteral("cannot connect to the Wayland display: %1")
                                   .arg(socketError());
        emit q->failed(reason);
        scheduleRetry();
        return false;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        handleDisconnect(QStringLiteral("cannot obtain the Wayland registry"));
        return false;
    }
    wl_registry_add_listener(registry, &s_registryListener, this);

    // First round trip: collect globals without binding, so ext wins over the
    // wlr protocol when a compositor advertises both (KWin, recent Hyprland).
    collectingGlobals = true;
    if (wl_display_roundtrip(display) < 0) {
        collectingGlobals = false;
        handleDisconnect(QStringLiteral("Wayland round trip failed: %1").arg(socketError()));
        return false;
    }
    collectingGlobals = false;
    ensureObjects();

    if (!manager || !device) {
        const QString reason = QStringLiteral(
            "the compositor offers neither ext-data-control-v1 nor "
            "wlr-data-control-unstable-v1");
        if (!warnedNoProtocol) {
            warnedNoProtocol = true;
            emit q->failed(reason);
        }
        teardown();
        scheduleRetry(30000);
        return false;
    }

    // Second round trip: the device advertises the current selection right
    // after it is created.
    if (wl_display_roundtrip(display) < 0) {
        handleDisconnect(QStringLiteral("Wayland round trip failed: %1").arg(socketError()));
        return false;
    }

    installNotifiers();
    qInfo() << "[WaylandClipboard] connected using" << backend;
    emit q->started();
    return true;
}

void WaylandClipboard::Impl::teardown()
{
    cancelTransfer();
    cancelSends();

    const QList<Source *> liveSources = sources.values();
    for (Source *source : liveSources)
        destroySource(source);
    sources.clear();

    destroyOffer(selection);
    selection = nullptr;
    destroyOffer(pendingOffer);
    pendingOffer = nullptr;

    if (device) {
        if (ext)
            ext_data_control_device_v1_destroy(reinterpret_cast<ext_data_control_device_v1 *>(device));
        else
            zwlr_data_control_device_v1_destroy(reinterpret_cast<zwlr_data_control_device_v1 *>(device));
        device = nullptr;
    }
    if (manager) {
        if (ext)
            ext_data_control_manager_v1_destroy(reinterpret_cast<ext_data_control_manager_v1 *>(manager));
        else
            zwlr_data_control_manager_v1_destroy(reinterpret_cast<zwlr_data_control_manager_v1 *>(manager));
        manager = nullptr;
    }

    if (readNotifier) {
        readNotifier->setEnabled(false);
        QObject::disconnect(readNotifier, nullptr, q, nullptr);
        readNotifier->deleteLater();
        readNotifier = nullptr;
    }
    if (writeNotifier) {
        writeNotifier->setEnabled(false);
        QObject::disconnect(writeNotifier, nullptr, q, nullptr);
        writeNotifier->deleteLater();
        writeNotifier = nullptr;
    }

    if (seat) {
        wl_seat_destroy(seat);
        seat = nullptr;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = nullptr;
    }
    if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
        display = nullptr;
    }

    ext = false;
    backend.clear();
    extName = wlrName = seatName = 0;
}

void WaylandClipboard::Impl::scheduleRetry(int delayMs)
{
    if (retryScheduled)
        return;
    retryScheduled = true;
    QTimer::singleShot(delayMs, q, [this] {
        retryScheduled = false;
        if (!display)
            q->start();
    });
}

void WaylandClipboard::Impl::handleDisconnect(const QString &reason)
{
    const bool wasConnected = display != nullptr;
    teardown();
    if (wasConnected)
        emit q->failed(reason);
    scheduleRetry();
}

// ============================================================================
// Impl: registry
// ============================================================================

void WaylandClipboard::Impl::handleGlobal(uint32_t name, const char *interface, uint32_t version)
{
    const QByteArray iface(interface);

    if (iface == QByteArrayLiteral("ext_data_control_manager_v1")) {
        extName = name;
        extVersion = 1;
    } else if (iface == QByteArrayLiteral("zwlr_data_control_manager_v1")) {
        wlrName = name;
        wlrVersion = qMin<uint32_t>(version, 2);
    } else if (iface == QByteArrayLiteral("wl_seat") && !seat) {
        seatName = name;
        seatVersion = qMin<uint32_t>(version, 7);
    } else {
        return;
    }

    if (!collectingGlobals)
        ensureObjects();
}

void WaylandClipboard::Impl::handleGlobalRemove(uint32_t name)
{
    if (name != seatName)
        return;

    // The seat disappeared: the data device becomes inert.  Drop it and wait
    // for a replacement seat.
    seatName = 0;
    if (device) {
        if (ext)
            ext_data_control_device_v1_destroy(reinterpret_cast<ext_data_control_device_v1 *>(device));
        else
            zwlr_data_control_device_v1_destroy(reinterpret_cast<zwlr_data_control_device_v1 *>(device));
        device = nullptr;
    }
    if (seat) {
        wl_seat_destroy(seat);
        seat = nullptr;
    }
}

void WaylandClipboard::Impl::ensureObjects()
{
    if (!display || !registry)
        return;

    if (!manager && (extName || wlrName)) {
        // Prefer the standardised ext protocol when both are advertised
        // (KWin exposes both ext-data-control-v1 and the wlr one).
        if (extName) {
            ext = true;
            backend = QStringLiteral("ext-data-control-v1");
            manager = static_cast<wl_proxy *>(
                wl_registry_bind(registry, extName,
                                 &ext_data_control_manager_v1_interface,
                                 extVersion));
        } else {
            ext = false;
            backend = QStringLiteral("wlr-data-control-unstable-v1");
            manager = static_cast<wl_proxy *>(
                wl_registry_bind(registry, wlrName,
                                 &zwlr_data_control_manager_v1_interface,
                                 wlrVersion));
        }
    }

    if (!seat && seatName) {
        seat = static_cast<wl_seat *>(
            wl_registry_bind(registry, seatName, &wl_seat_interface, seatVersion));
    }

    if (manager && seat && !device) {
        if (ext) {
            device = reinterpret_cast<wl_proxy *>(
                ext_data_control_manager_v1_get_data_device(
                    reinterpret_cast<ext_data_control_manager_v1 *>(manager), seat));
            if (device)
                ext_data_control_device_v1_add_listener(
                    reinterpret_cast<ext_data_control_device_v1 *>(device),
                    &s_extDeviceListener, this);
        } else {
            device = reinterpret_cast<wl_proxy *>(
                zwlr_data_control_manager_v1_get_data_device(
                    reinterpret_cast<zwlr_data_control_manager_v1 *>(manager), seat));
            if (device)
                zwlr_data_control_device_v1_add_listener(
                    reinterpret_cast<zwlr_data_control_device_v1 *>(device),
                    &s_wlrDeviceListener, this);
        }
    }
}

// ============================================================================
// Impl: dispatch
// ============================================================================

void WaylandClipboard::Impl::installNotifiers()
{
    if (readNotifier || !display)
        return;

    readNotifier = new QSocketNotifier(wl_display_get_fd(display),
                                       QSocketNotifier::Read, q);
    QObject::connect(readNotifier, &QSocketNotifier::activated, q,
                     [this](QSocketDescriptor, QSocketNotifier::Type) { onReadable(); });

    writeNotifier = new QSocketNotifier(wl_display_get_fd(display),
                                        QSocketNotifier::Write, q);
    writeNotifier->setEnabled(false);
    QObject::connect(writeNotifier, &QSocketNotifier::activated, q,
                     [this](QSocketDescriptor, QSocketNotifier::Type) { onWritable(); });

    flush();
}

void WaylandClipboard::Impl::flush()
{
    if (!display)
        return;

    if (wl_display_flush(display) < 0) {
        if (errno == EAGAIN) {
            if (writeNotifier)
                writeNotifier->setEnabled(true);
            return;
        }
        handleDisconnect(QStringLiteral("Wayland connection lost while flushing: %1")
                             .arg(socketError()));
    }
}

void WaylandClipboard::Impl::onWritable()
{
    if (!display)
        return;
    if (wl_display_flush(display) >= 0) {
        if (writeNotifier)
            writeNotifier->setEnabled(false);
    } else if (errno != EAGAIN) {
        handleDisconnect(QStringLiteral("Wayland connection lost: %1").arg(socketError()));
    }
}

void WaylandClipboard::Impl::onReadable()
{
    if (!display)
        return;

    // Standard libwayland Qt integration: prepare_read() fails when events
    // are already queued, in which case they must be dispatched instead of
    // reading the socket.
    if (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
            handleDisconnect(QStringLiteral("Wayland dispatch failed: %1").arg(socketError()));
            return;
        }
        flush();
        return;
    }

    // read_events() blocks when the socket is not readable; a zero-timeout
    // poll makes that decision race-free.
    struct pollfd pfd = { wl_display_get_fd(display), POLLIN, 0 };
    const int ready = ::poll(&pfd, 1, 0);
    if (ready <= 0) {
        wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
        if (ready < 0 && errno != EINTR && errno != EAGAIN)
            handleDisconnect(QStringLiteral("Wayland poll failed: %1").arg(socketError()));
        return;
    }

    if (wl_display_read_events(display) < 0) {
        handleDisconnect(QStringLiteral("Wayland connection lost: %1").arg(socketError()));
        return;
    }
    if (wl_display_dispatch_pending(display) < 0) {
        handleDisconnect(QStringLiteral("Wayland dispatch failed: %1").arg(socketError()));
        return;
    }
    flush();
}

// ============================================================================
// Impl: device events
// ============================================================================

void WaylandClipboard::Impl::handleDataOffer(wl_proxy *proxy)
{
    if (!proxy)
        return;

    // A data_offer is always followed by its offer events and then by either
    // selection or primary_selection, which consumes it.
    destroyOffer(pendingOffer);

    auto *offer = new Offer;
    offer->impl = this;
    offer->proxy = proxy;
    offer->ext = ext;
    pendingOffer = offer;

    if (ext) {
        ext_data_control_offer_v1_add_listener(
            reinterpret_cast<ext_data_control_offer_v1 *>(proxy), &s_extOfferListener, offer);
    } else {
        zwlr_data_control_offer_v1_add_listener(
            reinterpret_cast<zwlr_data_control_offer_v1 *>(proxy), &s_wlrOfferListener, offer);
    }
}

WaylandClipboard::Impl::Offer *WaylandClipboard::Impl::takePendingOffer(wl_proxy *proxy)
{
    if (pendingOffer && pendingOffer->proxy == proxy) {
        Offer *offer = pendingOffer;
        pendingOffer = nullptr;
        return offer;
    }
    return nullptr;
}

void WaylandClipboard::Impl::handleSelection(wl_proxy *proxy)
{
    // Whatever was being received belongs to the old selection.
    cancelTransfer();

    destroyOffer(selection);
    selection = nullptr;

    if (!proxy) {
        // Destroy a still-pending offer (selection with no offer should not
        // happen, but do not leak it).
        destroyOffer(pendingOffer);
        pendingOffer = nullptr;
        emit q->selectionUnset();
        return;
    }

    Offer *offer = takePendingOffer(proxy);
    if (!offer) {
        // Offer events were missed (should not happen); request the MIME
        // types again is impossible, so ignore this selection.
        return;
    }

    selection = offer;
    chooseOffer(offer);
}

void WaylandClipboard::Impl::handlePrimarySelection(wl_proxy *proxy)
{
    // Primary selection is not mirrored into the history, but its offer must
    // be destroyed to avoid leaking proxies.
    Offer *offer = takePendingOffer(proxy);
    if (offer)
        destroyOffer(offer);
}

void WaylandClipboard::Impl::handleDeviceFinished()
{
    handleDisconnect(QStringLiteral("the compositor invalidated the data device"));
}

// ============================================================================
// Impl: offers / incoming data
// ============================================================================

void WaylandClipboard::Impl::addMime(Offer *offer, const char *mime)
{
    if (!offer || !mime)
        return;
    const QByteArray value(mime);
    if (!offer->mimes.contains(value))
        offer->mimes.append(value);
}

void WaylandClipboard::Impl::chooseOffer(Offer *offer)
{
    QByteArray mime;
    int kind = 0;

    for (const QByteArray &candidate : textMimeCandidates()) {
        if (offer->mimes.contains(candidate)) {
            mime = candidate;
            kind = 1;
            break;
        }
    }

    // A source offering e.g. both text/html and image/png was most likely a
    // copied image; text/plain wins when it exists (the loop above).
    if (kind == 0) {
        for (const QByteArray &candidate : imageMimeCandidates()) {
            if (offer->mimes.contains(candidate)) {
                mime = candidate;
                kind = 2;
                break;
            }
        }
    }

    if (kind == 0)
        return; // nothing we can store

    requestData(offer, mime, kind);
}

void WaylandClipboard::Impl::requestData(Offer *offer, const QByteArray &mime, int kind)
{
    int fds[2] = { -1, -1 };
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        qWarning() << "[WaylandClipboard] pipe2 failed:" << socketError();
        return;
    }

    // Only the read end is non-blocking; the write end is handed to the
    // source process and must stay blocking there.
    const int flags = ::fcntl(fds[0], F_GETFL);
    if (flags >= 0)
        ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    if (ext) {
        ext_data_control_offer_v1_receive(
            reinterpret_cast<ext_data_control_offer_v1 *>(offer->proxy),
            mime.constData(), fds[1]);
    } else {
        zwlr_data_control_offer_v1_receive(
            reinterpret_cast<zwlr_data_control_offer_v1 *>(offer->proxy),
            mime.constData(), fds[1]);
    }

    // libwayland dup()s the descriptor while marshalling the request, so our
    // own copy of the write end must be closed here.  Keeping it open would
    // prevent the pipe from ever reaching EOF and the transfer would hang
    // after the data had been read.
    ::close(fds[1]);
    flush();

    transferFd = fds[0];
    transferData.clear();
    transferMime = mime;
    transferKind = kind;

    transferNotifier = new QSocketNotifier(transferFd, QSocketNotifier::Read, q);
    QObject::connect(transferNotifier, &QSocketNotifier::activated, q,
                     [this](QSocketDescriptor, QSocketNotifier::Type) { onTransferReadable(); });
}

void WaylandClipboard::Impl::onTransferReadable()
{
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(transferFd, buffer, sizeof buffer);
        if (n > 0) {
            if (transferData.size() + n > kMaxTransferBytes) {
                qWarning() << "[WaylandClipboard] clipboard transfer exceeded"
                           << kMaxTransferBytes << "bytes; dropped";
                cancelTransfer();
                return;
            }
            transferData.append(buffer, static_cast<int>(n));
            continue;
        }
        if (n == 0) {
            finishTransfer();
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        qWarning() << "[WaylandClipboard] clipboard read failed:" << socketError();
        cancelTransfer();
        return;
    }
}

void WaylandClipboard::Impl::finishTransfer()
{
    const QByteArray data = transferData;
    const QByteArray mime = transferMime;
    const int kind = transferKind;
    cancelTransfer();

    if (data.isEmpty())
        return;

    if (kind == 1) {
        QString text;
        if (mime == QByteArrayLiteral("STRING") || mime == QByteArrayLiteral("TEXT"))
            text = QString::fromLatin1(data); // legacy X11 encodings
        else
            text = QString::fromUtf8(data);
        emit q->textReceived(text);
    } else if (kind == 2) {
        emit q->imageReceived(data, QString::fromLatin1(mime));
    }
}

void WaylandClipboard::Impl::cancelTransfer()
{
    if (transferNotifier) {
        transferNotifier->setEnabled(false);
        QObject::disconnect(transferNotifier, nullptr, q, nullptr);
        transferNotifier->deleteLater();
        transferNotifier = nullptr;
    }
    if (transferFd >= 0) {
        ::close(transferFd);
        transferFd = -1;
    }
    transferData.clear();
    transferMime.clear();
    transferKind = 0;
}

void WaylandClipboard::Impl::destroyOffer(Offer *offer)
{
    if (!offer)
        return;
    if (offer->proxy) {
        if (offer->ext)
            ext_data_control_offer_v1_destroy(
                reinterpret_cast<ext_data_control_offer_v1 *>(offer->proxy));
        else
            zwlr_data_control_offer_v1_destroy(
                reinterpret_cast<zwlr_data_control_offer_v1 *>(offer->proxy));
    }
    delete offer;
}

// ============================================================================
// Impl: sources / outgoing data
// ============================================================================

void WaylandClipboard::Impl::installSource(const QByteArray &data,
                                           const QList<QByteArray> &mimes)
{
    if (!manager || !device || mimes.isEmpty())
        return;

    auto *source = new Source;
    source->impl = this;
    source->ext = ext;
    source->data = data;

    if (ext) {
        auto *proxy = ext_data_control_manager_v1_create_data_source(
            reinterpret_cast<ext_data_control_manager_v1 *>(manager));
        if (!proxy) {
            delete source;
            return;
        }
        source->proxy = reinterpret_cast<wl_proxy *>(proxy);
        ext_data_control_source_v1_add_listener(proxy, &s_extSourceListener, source);
        for (const QByteArray &mime : mimes)
            ext_data_control_source_v1_offer(proxy, mime.constData());
        ext_data_control_device_v1_set_selection(
            reinterpret_cast<ext_data_control_device_v1 *>(device), proxy);
    } else {
        auto *proxy = zwlr_data_control_manager_v1_create_data_source(
            reinterpret_cast<zwlr_data_control_manager_v1 *>(manager));
        if (!proxy) {
            delete source;
            return;
        }
        source->proxy = reinterpret_cast<wl_proxy *>(proxy);
        zwlr_data_control_source_v1_add_listener(proxy, &s_wlrSourceListener, source);
        for (const QByteArray &mime : mimes)
            zwlr_data_control_source_v1_offer(proxy, mime.constData());
        zwlr_data_control_device_v1_set_selection(
            reinterpret_cast<zwlr_data_control_device_v1 *>(device), proxy);
    }

    sources.insert(source->proxy, source);

    // The compositor only replaces the previous source once it processed
    // set_selection; that previous source is cleaned up in
    // handleSourceCancelled().  Surviving sources stay alive because they
    // still provide the selection data to pasting clients.
    flush();
}

void WaylandClipboard::Impl::handleSourceSend(Source *source, const char *mime, int fd)
{
    Q_UNUSED(mime) // only MIME types we advertised are requested

    if (!source || fd < 0) {
        if (fd >= 0)
            ::close(fd);
        return;
    }

    // Only the receiving side may block; this process writes from the event
    // loop so a large payload never stalls the GUI thread (see Impl::Send).
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    auto *send = new Send;
    // Shared, not copied: the source may be replaced (another client takes the
    // selection) while the transfer is still in flight.
    send->data = source->data;
    send->fd = fd;
    send->notifier = new QSocketNotifier(fd, QSocketNotifier::Write, q);
    QObject::connect(send->notifier, &QSocketNotifier::activated, q,
                     [this, send](QSocketDescriptor, QSocketNotifier::Type) { onSendWritable(send); });
    sends.insert(send);

    // Text fits into the pipe buffer, so this already finishes most transfers
    // without ever enabling the notifier.
    onSendWritable(send);
}

void WaylandClipboard::Impl::onSendWritable(Send *send)
{
    if (!send)
        return;

    while (send->offset < send->data.size()) {
        const ssize_t n = ::write(send->fd, send->data.constData() + send->offset,
                                  static_cast<size_t>(send->data.size() - send->offset));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return; // pipe full: the notifier resumes when the reader drains
            finishSend(send); // EPIPE: the pasting client went away
            return;
        }
        send->offset += n;
    }
    finishSend(send);
}

void WaylandClipboard::Impl::finishSend(Send *send)
{
    if (!send)
        return;
    sends.remove(send);
    if (send->notifier) {
        send->notifier->setEnabled(false);
        QObject::disconnect(send->notifier, nullptr, q, nullptr);
        send->notifier->deleteLater();
    }
    if (send->fd >= 0)
        ::close(send->fd);
    delete send;
}

void WaylandClipboard::Impl::cancelSends()
{
    const QList<Send *> live(sends.begin(), sends.end());
    for (Send *send : live)
        finishSend(send);
    sends.clear();
}

void WaylandClipboard::Impl::handleSourceCancelled(Source *source)
{
    // Another client (or we ourselves) replaced the selection.
    destroySource(source);
}

void WaylandClipboard::Impl::destroySource(Source *source)
{
    if (!source)
        return;
    sources.remove(source->proxy);
    if (source->proxy) {
        if (source->ext)
            ext_data_control_source_v1_destroy(
                reinterpret_cast<ext_data_control_source_v1 *>(source->proxy));
        else
            zwlr_data_control_source_v1_destroy(
                reinterpret_cast<zwlr_data_control_source_v1 *>(source->proxy));
        source->proxy = nullptr;
    }
    delete source;
}

// ============================================================================
// WaylandClipboard
// ============================================================================

WaylandClipboard::WaylandClipboard(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>(this))
{
    // Serving a paste is best effort: if the requesting client disappears
    // mid-transfer, write() raises SIGPIPE and would kill the whole shell.
    // wl-clipboard does the same.
    std::signal(SIGPIPE, SIG_IGN);

    connect(this, &WaylandClipboard::failed, this, [](const QString &reason) {
        qWarning() << "[WaylandClipboard]" << reason;
    });
}

WaylandClipboard::~WaylandClipboard() = default;

bool WaylandClipboard::start()
{
    return d->connectDisplay();
}

bool WaylandClipboard::asyncSend() const
{
    // Serving the selection never runs a blocking write any more, so a
    // multi-megabyte payload (screenshot) cannot stall the event loop.
    return true;
}

bool WaylandClipboard::isAvailable() const
{
    return d->display != nullptr && d->manager != nullptr && d->device != nullptr;
}

QString WaylandClipboard::backendName() const
{
    return d->backend;
}

void WaylandClipboard::setText(const QString &text)
{
    d->installSource(text.toUtf8(),
                     { QByteArrayLiteral("text/plain;charset=utf-8"),
                       QByteArrayLiteral("text/plain"),
                       QByteArrayLiteral("UTF8_STRING"),
                       QByteArrayLiteral("TEXT"),
                       QByteArrayLiteral("STRING") });
}

void WaylandClipboard::setImage(const QByteArray &data, const QString &mime)
{
    const QByteArray type = mime.isEmpty() ? QByteArrayLiteral("image/png")
                                           : mime.toLatin1();
    d->installSource(data, { type });
}
