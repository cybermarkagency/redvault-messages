#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "redvault/net/curl_transport.hpp"

struct RvTorGateway;

namespace redvault::app {

/**
 * The embedded Tor gateway (tor-core, docs/tor-routing-plan.md), for the
 * whole app run. Routing goes through it whenever the server address is a
 * .onion; then text, photos, files, voice messages, stories and everything
 * they depend on have no other way out. Calls are not routed here (WebRTC +
 * TURN).
 *
 * `state` for the UI: "off" (not a Tor build/run), "connecting", "connected"
 * or "unavailable". While it is "unavailable" the transport's gate() returns
 * nothing and every request fails before a socket opens; while "connecting",
 * requests wait inside Tor. Sends go through the outbox, which only drains
 * once Tor is "connected".
 */
class TorController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by main()")
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(QString detail READ detail NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(QString onionHost READ onionHost CONSTANT)
    Q_PROPERTY(int queued READ queued NOTIFY queuedChanged)
    Q_PROPERTY(int pausedUploads READ pausedUploads NOTIFY queuedChanged)

public:
    /// `onionHost` empty: Tor routing is off for this run (clearnet build talking to a clearnet server).
    TorController(QString onionHost, QString stateDir, QObject* parent = nullptr);
    ~TorController() override;

    bool enabled() const { return !onionHost_.isEmpty(); }
    QString state() const { return state_; }
    int progress() const { return progress_; }
    QString detail() const { return detail_; }
    bool connected() const { return state_ == QStringLiteral("connected"); }
    QString onionHost() const { return onionHost_; }
    int queued() const { return queued_; }
    int pausedUploads() const { return pausedUploads_; }

    /// What the outbox holds (for the status chip): messages and, of those, files.
    void setQueued(int messages, int files);

    /// For CurlTransport::Options::torGate: thread-safe, empty unless connected.
    std::function<std::optional<net::SocksEndpoint>()> gate() const;

    /// Closes the SOCKS port and stops Tor (quitting).
    void stop();

signals:
    void changed();
    void queuedChanged();
    /// Tor just became usable: time to send what's queued.
    void becameConnected();

private:
    void poll();

    struct Shared {
        std::mutex mutex;
        std::optional<net::SocksEndpoint> endpoint;
        bool usable = false;
    };

    QString onionHost_;
    QString state_ = QStringLiteral("off");
    int progress_ = 0;
    QString detail_;
    int queued_ = 0;
    int pausedUploads_ = 0;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    RvTorGateway* gateway_ = nullptr;
    QTimer timer_;
};

}  // namespace redvault::app
