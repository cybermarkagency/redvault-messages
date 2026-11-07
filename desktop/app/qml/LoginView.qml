import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Redvault

// Sign in, create an account, or recover one; then (for accounts with
// two-factor on) the code step.
Item {
    id: root
    readonly property bool needsCode: App.state === App.NeedsCode
    // "signin", "register" or "recover"
    property string mode: "signin"
    // Sign-up: Create Account stays disabled until all three boxes are ticked.
    readonly property bool consentComplete: termsBox.checked && privacyBox.checked && ageBox.checked

    function setMode(next) {
        App.clearError()
        mode = next
        passwordField.text = ""
        confirmField.text = ""
        recoveryKeyField.text = ""
        recoveryCodeField.text = ""
        termsBox.checked = false
        privacyBox.checked = false
        ageBox.checked = false
        Qt.callLater(focusFirst)
    }

    function focusFirst() {
        if (root.needsCode)
            codeField.forceActiveFocus()
        else if (handleField.text.length > 0 && mode !== "register")
            (mode === "recover" ? recoveryKeyField : passwordField).forceActiveFocus()
        else
            handleField.forceActiveFocus()
    }

    function submit() {
        if (App.busy)
            return
        if (root.needsCode)
            App.submitCode(codeField.text)
        else if (mode === "register") {
            // Enter in a field without all boxes ticked gets AppController's explanation.
            App.registerAccount(handleField.text, passwordField.text, confirmField.text,
                                termsBox.checked, privacyBox.checked, ageBox.checked)
        }
        else if (mode === "recover")
            App.recover(handleField.text, recoveryKeyField.text, passwordField.text, confirmField.text, recoveryCodeField.text)
        else
            App.signIn(handleField.text, passwordField.text)
    }

    Flickable {
        anchors.fill: parent
        contentHeight: Math.max(height, card.height + 80)
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Rectangle {
            id: card
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(40, (parent.height - height) / 2)
            width: 400
            height: column.implicitHeight + 64
            radius: Theme.radiusCard
            color: Theme.surface
            border.color: Theme.border

            ColumnLayout {
                id: column
                anchors.fill: parent
                anchors.margins: 32
                spacing: 12

                Image {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 56
                    source: "qrc:/qt/qml/Redvault/icons/app/redvault.svg"
                    sourceSize: Qt.size(56, 56)
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.needsCode ? "Two-factor code"
                        : root.mode === "register" ? "Create your account"
                        : root.mode === "recover" ? "Recover your account"
                        : "Sign in to redvaultmessages"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.needsCode ? "Enter the 6-digit code from your authenticator app."
                        : root.mode === "register" ? "Your keys are created on this computer. Nobody can reset your password for you, so keep your recovery key safe."
                        : root.mode === "recover" ? "Use the recovery key you saved when you created your account, and choose a new password. Your other devices will be signed out."
                        : "Your messages are decrypted on this computer only. Nothing is saved when you quit."
                    textFormat: Text.PlainText
                    color: Theme.text2
                    font.pixelSize: Theme.fontSmall + 1
                }

                Item { Layout.preferredHeight: 2 }

                RvTextField {
                    id: handleField
                    visible: !root.needsCode
                    Layout.fillWidth: true
                    placeholderText: "Handle"
                    text: App.lastHandle
                    maximumLength: 64
                    enabled: !App.busy
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onAccepted: root.submit()
                }

                RvTextField {
                    id: recoveryKeyField
                    visible: !root.needsCode && root.mode === "recover"
                    Layout.fillWidth: true
                    placeholderText: "Recovery key (XXXX-XXXX-…)"
                    enabled: !App.busy
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
                    onAccepted: root.submit()
                }

                RvTextField {
                    id: passwordField
                    visible: !root.needsCode
                    Layout.fillWidth: true
                    placeholderText: root.mode === "signin" ? "Password" : root.mode === "recover" ? "New password (at least 8 characters)" : "Password (at least 8 characters)"
                    echoMode: TextInput.Password
                    enabled: !App.busy
                    onAccepted: root.submit()
                }

                RvTextField {
                    id: confirmField
                    visible: !root.needsCode && root.mode !== "signin"
                    Layout.fillWidth: true
                    placeholderText: "Repeat the password"
                    echoMode: TextInput.Password
                    enabled: !App.busy
                    onAccepted: root.submit()
                }

                // The three sign-up boxes; the links open the documents without ticking anything.
                ConsentBox {
                    id: termsBox
                    visible: !root.needsCode && root.mode === "register"
                    Layout.fillWidth: true
                    label: "I agree to the RedVault Messages <a href=\"terms\">Terms of Service</a>"
                    onLinkActivated: (link) => legalDialog.show(link)
                }
                ConsentBox {
                    id: privacyBox
                    visible: !root.needsCode && root.mode === "register"
                    Layout.fillWidth: true
                    label: "I acknowledge the RedVault Messages <a href=\"privacy\">Privacy Policy</a>"
                    onLinkActivated: (link) => legalDialog.show(link)
                }
                ConsentBox {
                    id: ageBox
                    visible: !root.needsCode && root.mode === "register"
                    Layout.fillWidth: true
                    label: "I confirm I am 18 or older"
                }

                RvTextField {
                    id: recoveryCodeField
                    visible: !root.needsCode && root.mode === "recover" && App.recoveryNeedsCode
                    Layout.fillWidth: true
                    placeholderText: "6-digit code from your authenticator app"
                    inputMethodHints: Qt.ImhDigitsOnly
                    maximumLength: 10
                    enabled: !App.busy
                    onAccepted: root.submit()
                }

                RvTextField {
                    id: codeField
                    visible: root.needsCode
                    Layout.fillWidth: true
                    placeholderText: "123456"
                    inputMethodHints: Qt.ImhDigitsOnly
                    maximumLength: 10
                    enabled: !App.busy
                    onAccepted: root.submit()
                }

                Banner {
                    Layout.fillWidth: true
                    text: App.error
                }

                RvButton {
                    Layout.fillWidth: true
                    enabled: !App.busy && (root.needsCode || root.mode !== "register" || root.consentComplete)
                    text: App.busy ? (root.needsCode ? "Checking…" : root.mode === "register" ? "Creating your keys…" : "Unlocking…")
                        : root.needsCode ? "Verify"
                        : root.mode === "register" ? "Create Account"
                        : root.mode === "recover" ? "Recover account"
                        : "Sign in"
                    onClicked: root.submit()

                    BusyIndicator {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 24
                        height: 24
                        running: App.busy
                        visible: App.busy
                    }
                }

                // Switching between modes.
                RvButton {
                    Layout.fillWidth: true
                    visible: root.needsCode
                    primary: false
                    text: "Back"
                    enabled: !App.busy
                    onClicked: {
                        codeField.text = ""
                        App.cancelCode()
                    }
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: !root.needsCode && root.mode === "signin"
                    primary: false
                    text: "Create an account"
                    enabled: !App.busy
                    onClicked: root.setMode("register")
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: !root.needsCode && root.mode === "signin"
                    primary: false
                    text: "Forgot your password?"
                    enabled: !App.busy
                    onClicked: root.setMode("recover")
                }
                RvButton {
                    Layout.fillWidth: true
                    visible: !root.needsCode && root.mode !== "signin"
                    primary: false
                    text: "Back to sign in"
                    enabled: !App.busy
                    onClicked: root.setMode("signin")
                }
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        text: "End-to-end encrypted · same account as the web and Android apps"
        color: Theme.text3
        font.pixelSize: Theme.fontSmall
    }

    LegalDocumentDialog {
        id: legalDialog
    }

    Component.onCompleted: focusFirst()

    onNeedsCodeChanged: {
        passwordField.text = ""
        if (needsCode)
            codeField.forceActiveFocus()
    }
}
