#pragma once

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/data/drive_repository.hpp"
#include "redvault/net/http.hpp"
#include "redvault/update/update.hpp"

namespace redvault::app {

/// What went wrong in a background call, already worded for the UI.
struct Failure {
    QString message;
    /// HTTP status for API errors (401 ends the session), 0 otherwise.
    int status = 0;
    QString code;
};

/// Turns any exception from the core into a Failure fit to show.
Failure describe(std::exception_ptr error);

/**
 * Runs `work` on `pool` (every core call blocks, and Argon2id is slow), then
 * delivers the result to `onDone` or the failure to `onFail` on the UI
 * thread, but only if `owner` still exists by then.
 *
 * `work` returns a value (moved to onDone) or void (onDone takes nothing).
 */
template <typename Work, typename Done, typename Fail>
void runInBackground(QThreadPool& pool, QObject* owner, Work work, Done onDone, Fail onFail) {
    using Result = std::invoke_result_t<Work&>;
    QPointer<QObject> guard(owner);
    pool.start([guard, work = std::move(work), onDone = std::move(onDone), onFail = std::move(onFail)]() mutable {
        auto failure = std::make_shared<std::optional<Failure>>();
        if constexpr (std::is_void_v<Result>) {
            try {
                work();
            } catch (...) {
                *failure = describe(std::current_exception());
            }
            // qApp as the context: always alive while events run; `guard` says whether the owner is.
            QMetaObject::invokeMethod(
                qApp,
                [guard, failure, onDone = std::move(onDone), onFail = std::move(onFail)]() mutable {
                    if (!guard) return;
                    if (*failure) {
                        onFail(**failure);
                    } else {
                        onDone();
                    }
                },
                Qt::QueuedConnection);
        } else {
            auto result = std::make_shared<std::optional<Result>>();
            try {
                result->emplace(work());
            } catch (...) {
                *failure = describe(std::current_exception());
            }
            QMetaObject::invokeMethod(
                qApp,
                [guard, failure, result, onDone = std::move(onDone), onFail = std::move(onFail)]() mutable {
                    if (!guard) return;
                    if (*failure) {
                        onFail(**failure);
                    } else {
                        onDone(std::move(**result));
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

inline Failure describe(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const api::ApiError& e) {
        return Failure{QString::fromUtf8(e.what()), e.status(), QString::fromStdString(e.code())};
    } catch (const data::TransferCancelled& e) {
        return Failure{QString::fromUtf8(e.what()), 0, QStringLiteral("cancelled")};
    } catch (const api::TransferInterrupted& e) {
        // No connection or Tor is down: pause and resume, don't call it failed.
        return Failure{QString::fromUtf8(e.what()), 0, QStringLiteral("interrupted")};
    } catch (const api::UserFacingError& e) {
        return Failure{QString::fromUtf8(e.what()), 0, {}};
    } catch (const update::UpdateError& e) {
        return Failure{QString::fromUtf8(e.what()), 0, {}};
    } catch (const net::NetworkError&) {
        return Failure{QStringLiteral("Can't reach the server. Check your connection."), 0, {}};
    } catch (const crypto::files::FileDecryptError& e) {
        return Failure{QString::fromUtf8(e.what()), 0, {}};
    } catch (const crypto::CryptoError&) {
        return Failure{QStringLiteral("Couldn't unlock your key. Check your password and try again."), 0, {}};
    } catch (const std::invalid_argument&) {
        return Failure{QStringLiteral("That can't be sent."), 0, {}};
    } catch (const std::exception&) {
        return Failure{QStringLiteral("Something went wrong. Try again."), 0, {}};
    } catch (...) {
        return Failure{QStringLiteral("Something went wrong. Try again."), 0, {}};
    }
}

}  // namespace redvault::app
