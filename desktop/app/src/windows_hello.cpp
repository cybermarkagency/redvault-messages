// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "windows_hello.hpp"

#include <QWindow>

#ifdef _WIN32
#include <initguid.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.security.credentials.ui.h>

namespace {

using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Security::Credentials::UI::IUserConsentVerifierStatics;
using ABI::Windows::Security::Credentials::UI::UserConsentVerificationResult;
using ABI::Windows::Security::Credentials::UI::UserConsentVerifierAvailability;

// From UserConsentVerifierInterop.h (Windows SDK), which MinGW doesn't ship:
// lets a desktop app show the Hello prompt over its own window.
const IID kInteropIid = {0x39E050C3, 0x4E74, 0x441A, {0x8D, 0xC0, 0xB8, 0x11, 0x04, 0xDF, 0x94, 0x9C}};
struct IUserConsentVerifierInterop : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestVerificationForWindowAsync(HWND appWindow, HSTRING message, REFIID riid,
                                                                       void** asyncOperation) = 0;
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

HSTRING hstring(const wchar_t* text) {
    HSTRING out = nullptr;
    WindowsCreateString(text, static_cast<UINT32>(wcslen(text)), &out);
    return out;
}

template <typename I>
bool factory(ComPtr<I>& out, REFIID iid) {
    HSTRING name = hstring(RuntimeClass_Windows_Security_Credentials_UI_UserConsentVerifier);
    const HRESULT hr = RoGetActivationFactory(name, iid, reinterpret_cast<void**>(out.put()));
    WindowsDeleteString(name);
    return SUCCEEDED(hr) && out;
}

}  // namespace

namespace redvault::app {

/// One outstanding WinRT async call, polled from the UI thread until it finishes.
struct WindowsHello::Pending {
    IUnknown* operation = nullptr;  // IAsyncOperation<...>
    std::function<void(IUnknown* operation, bool completed)> finish;
    ~Pending() {
        if (operation) operation->Release();
    }
};

WindowsHello::WindowsHello(QObject* parent) : QObject(parent) {
    // WinRT needs the thread initialised; Qt has usually done it already (then this is a no-op).
    RoInitialize(RO_INIT_SINGLETHREADED);
    poll_.setInterval(50);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!pending_) {
            poll_.stop();
            return;
        }
        ComPtr<IAsyncInfo> info;
        if (FAILED(pending_->operation->QueryInterface(IID_IAsyncInfo, reinterpret_cast<void**>(info.put())))) {
            auto* done = pending_;
            pending_ = nullptr;
            poll_.stop();
            done->finish(done->operation, false);
            delete done;
            return;
        }
        AsyncStatus status = AsyncStatus::Started;
        info->get_Status(&status);
        if (status == AsyncStatus::Started) return;
        auto* done = pending_;
        pending_ = nullptr;
        poll_.stop();
        done->finish(done->operation, status == AsyncStatus::Completed);
        delete done;
    });
}

WindowsHello::~WindowsHello() {
    delete pending_;
}

void WindowsHello::checkAvailability(std::function<void(bool)> done) {
    ComPtr<IUserConsentVerifierStatics> statics;
    if (!factory(statics, IID___x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics) || pending_) {
        done(false);
        return;
    }
    IAsyncOperation<UserConsentVerifierAvailability>* operation = nullptr;
    if (FAILED(statics->CheckAvailabilityAsync(&operation)) || !operation) {
        done(false);
        return;
    }
    pending_ = new Pending{operation, [done](IUnknown* op, bool completed) {
                               UserConsentVerifierAvailability availability = UserConsentVerifierAvailability::UserConsentVerifierAvailability_DeviceNotPresent;
                               if (completed) static_cast<IAsyncOperation<UserConsentVerifierAvailability>*>(op)->GetResults(&availability);
                               done(completed && availability == UserConsentVerifierAvailability::UserConsentVerifierAvailability_Available);
                           }};
    poll_.start();
}

void WindowsHello::verify(QWindow* window, const QString& message, std::function<void(bool, QString)> done) {
    if (pending_) {
        done(false, QStringLiteral("Windows Hello is already asking."));
        return;
    }
    ComPtr<IUserConsentVerifierInterop> interop;
    if (!factory(interop, kInteropIid)) {
        done(false, QStringLiteral("Windows Hello isn't available on this PC."));
        return;
    }
    HSTRING text = hstring(reinterpret_cast<const wchar_t*>(message.utf16()));
    IAsyncOperation<UserConsentVerificationResult>* operation = nullptr;
    const HRESULT hr = interop->RequestVerificationForWindowAsync(window ? reinterpret_cast<HWND>(window->winId()) : nullptr, text,
                                                                 IID___FIAsyncOperation_1_UserConsentVerificationResult,
                                                                 reinterpret_cast<void**>(&operation));
    WindowsDeleteString(text);
    if (FAILED(hr) || !operation) {
        done(false, QStringLiteral("Windows Hello couldn't start."));
        return;
    }
    pending_ = new Pending{operation, [done](IUnknown* op, bool completed) {
                               UserConsentVerificationResult result = UserConsentVerificationResult::UserConsentVerificationResult_Canceled;
                               if (completed) static_cast<IAsyncOperation<UserConsentVerificationResult>*>(op)->GetResults(&result);
                               if (result == UserConsentVerificationResult::UserConsentVerificationResult_Verified) {
                                   done(true, {});
                               } else if (result == UserConsentVerificationResult::UserConsentVerificationResult_Canceled) {
                                   done(false, {});
                               } else {
                                   done(false, QStringLiteral("Windows Hello didn't verify you. Try again, or use your password."));
                               }
                           }};
    poll_.start();
}

}  // namespace redvault::app

#else  // Linux: no Hello.

namespace redvault::app {

struct WindowsHello::Pending {};

WindowsHello::WindowsHello(QObject* parent) : QObject(parent) {}
WindowsHello::~WindowsHello() = default;

void WindowsHello::checkAvailability(std::function<void(bool)> done) {
    done(false);
}

void WindowsHello::verify(QWindow*, const QString&, std::function<void(bool, QString)> done) {
    done(false, QStringLiteral("Windows Hello is only on Windows."));
}

}  // namespace redvault::app

#endif
