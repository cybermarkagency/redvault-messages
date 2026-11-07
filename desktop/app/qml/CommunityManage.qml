pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import Redvault

// Managing a community, as the web app's CommunityManage: publish,
// unpublish, close; the key epoch and "Deliver keys now"; settings and
// price versions; members (approve, roles, posting rights, remove, ban,
// unban); one-time invite links; reports. Staff only.
ColumnLayout {
    id: manage
    required property CommunitySession communities
    spacing: 14

    readonly property var c: communities.community
    readonly property bool loaded: c.id !== undefined
    readonly property var staff: loaded ? c.staff : null
    readonly property string role: loaded && c.viewer.membership ? c.viewer.membership.role : "member"
    readonly property var rank: ({ owner: 3, admin: 2, moderator: 1, member: 0 })
    readonly property bool canEdit: role === "owner" || role === "admin"
    property string section: "settings"
    property var acting: null

    function show(next) {
        section = next
        if (next === "members") communities.loadMembers(communities.memberFilter)
        if (next === "invitations") communities.loadInvitations()
        if (next === "reports") communities.loadReports()
    }

    ConfirmDialog {
        id: closeDialog
        title: "Close this community?"
        message: manage.loaded && manage.c.accessType === "paid"
                 ? "Nobody is charged again. Paying members keep access until their paid period ends, then it closes for good. Refunds, if any, are handled by RedVault support."
                 : "Everyone except you loses access now."
        confirmText: "Close community"
        danger: true
        onConfirmed: manage.communities.closeCommunity()
    }
    ConfirmDialog {
        id: removeDialog
        title: "Remove this member?"
        message: manage.acting ? "Remove " + manage.acting.username + "? They lose access now" + (manage.acting.entitlement ? " and their membership stops billing (no automatic refund)" : "")
                                 + ". The next post switches to a new key they never get." : ""
        confirmText: "Remove"
        danger: true
        onConfirmed: manage.communities.removeMember(manage.acting.userId, false, "")
    }
    RvDialog {
        id: banDialog
        title: "Ban this member?"
        onOpened: banReason.text = ""
        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 12
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text2
                text: manage.acting ? "Ban " + manage.acting.username + "? They can't rejoin or subscribe again." : ""
                textFormat: Text.PlainText
            }
            RvTextField {
                id: banReason
                Layout.fillWidth: true
                maximumLength: 200
                placeholderText: "Reason (optional)"
                Accessible.name: "Reason"
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                RvButton {
                    primary: false
                    text: "Cancel"
                    onClicked: banDialog.close()
                }
                RvButton {
                    danger: true
                    text: "Ban"
                    onClicked: {
                        manage.communities.removeMember(manage.acting.userId, true, banReason.text)
                        banDialog.close()
                    }
                }
            }
        }
    }

    RvButton {
        primary: false
        iconName: "chevron-left"
        text: "Community page"
        leftPadding: 6
        onClicked: manage.communities.open("community", manage.communities.communityId)
    }
    Text {
        visible: !manage.loaded && manage.communities.communityError.length === 0
        text: "Loading…"
        color: Theme.text3
    }
    Banner {
        Layout.fillWidth: true
        text: manage.communities.communityError.length > 0 ? manage.communities.communityError
            : manage.loaded && !manage.staff ? "Only the creator and staff manage this community." : ""
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: manage.loaded && !!manage.staff
        spacing: 14

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: manage.loaded ? manage.c.name : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text
                font.family: Theme.displayFamily
                font.pixelSize: 24
                font.weight: Font.Bold
            }
            Text {
                text: manage.loaded ? (manage.c.kind === "channel" ? "Channel" : "Group") + " · " + (manage.c.accessType === "paid" ? "Paid" : "Free") + " · you're " + manage.role : ""
                textFormat: Text.PlainText
                color: Theme.text3
                font.pixelSize: 13
            }
        }

        // Lifecycle and keys.
        Card {
            Layout.fillWidth: true
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Pill {
                    text: manage.loaded ? manage.c.status : ""
                    tone: manage.loaded && manage.c.status === "published" ? "accent" : "warn"
                }
                Pill { text: manage.loaded && manage.c.visibility === "discoverable" ? "Listed in Discover" : "Unlisted" }
                Pill {
                    visible: manage.loaded && !!manage.c.price
                    text: manage.loaded && manage.c.price ? manage.communities.formatMoney(manage.c.price.amountMinor, manage.c.price.currency) + "/month" : ""
                }
                Pill { text: manage.staff ? "Key epoch " + manage.staff.keyEpoch : "" }
                Pill {
                    visible: !!manage.staff && manage.staff.rekeyRequired === true
                    tone: "warn"
                    text: "New key needed"
                }
            }
            Text {
                Layout.fillWidth: true
                visible: !!manage.staff && manage.staff.rekeyRequired === true
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: 13
                text: "Someone left, so the next message switches to a new key that only current members get. Any posting member's app does this automatically."
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                RvButton {
                    visible: manage.role === "owner" && manage.loaded && (manage.c.status === "draft" || manage.c.status === "unpublished")
                    text: "Publish"
                    enabled: !manage.communities.busy
                    onClicked: manage.communities.publish()
                }
                RvButton {
                    visible: manage.role === "owner" && manage.loaded && manage.c.status === "published"
                    secondary: true
                    text: "Unpublish"
                    enabled: !manage.communities.busy
                    onClicked: manage.communities.unpublish()
                }
                RvButton {
                    secondary: true
                    iconName: "key"
                    text: "Deliver keys now"
                    enabled: !manage.communities.busy
                    onClicked: manage.communities.deliverKeysNow()
                }
                RvButton {
                    visible: manage.role === "owner" && manage.loaded && manage.c.status !== "closed" && manage.c.status !== "closing"
                    primary: false
                    danger: true
                    text: "Close community"
                    enabled: !manage.communities.busy
                    onClicked: closeDialog.open()
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [["settings", "Settings"], ["members", "Members"], ["invitations", "Invitations"], ["reports", "Reports"]]
                delegate: Chip {
                    required property var modelData
                    text: modelData[1]
                    checked: manage.section === modelData[0]
                    onClicked: manage.show(modelData[0])
                }
            }
        }

        // --- Settings ---
        Text {
            Layout.fillWidth: true
            visible: manage.section === "settings" && !manage.canEdit
            wrapMode: Text.Wrap
            color: Theme.text3
            text: "Moderators manage members, invitations and reports. Settings are for the creator and admins."
        }
        ColumnLayout {
            id: settings
            Layout.fillWidth: true
            visible: manage.section === "settings" && manage.canEdit
            spacing: 10

            // The form starts from the community as last loaded (and again after each save).
            readonly property int version: manage.staff ? manage.staff.version : 0
            onVersionChanged: fill()
            Component.onCompleted: fill()
            function fill() {
                if (!manage.loaded || !manage.staff) return
                name.text = manage.c.name
                description.text = manage.c.description
                category.value = manage.c.category
                visibility.value = manage.c.visibility
                memberLimit.text = manage.staff.memberLimit ? String(manage.staff.memberLimit) : ""
                slowMode.value = manage.staff.slowModeSeconds
                rules.text = manage.c.rules
                preview.text = manage.c.previewText
                paused.checked = manage.c.newMembersPaused === true
                invitesOff.checked = manage.staff.invitationsSuspended === true
                approval.checked = manage.staff.joinApproval === true
                price.text = manage.c.price ? (manage.c.price.amountMinor / 100).toFixed(2) : ""
            }

            FieldLabel { text: "Name" }
            RvTextField {
                id: name
                Layout.fillWidth: true
                maximumLength: 80
                Accessible.name: "Name"
            }
            FieldLabel { text: "Description" }
            PostField {
                id: description
                Layout.fillWidth: true
                limit: 2000
                Accessible.name: "Description"
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 10
                rowSpacing: 6
                FieldLabel { text: "Category" }
                FieldLabel { text: "Who can find it" }
                RvComboBox {
                    id: category
                    Layout.fillWidth: true
                    Accessible.name: "Category"
                    options: manage.communities.categories
                }
                RvComboBox {
                    id: visibility
                    Layout.fillWidth: true
                    Accessible.name: "Who can find it"
                    options: [{ value: "unlisted", label: "Unlisted: invite link only" }, { value: "discoverable", label: "Listed in Discover" }]
                }
                FieldLabel { text: "Member limit" }
                FieldLabel { text: "Slow mode" }
                RvTextField {
                    id: memberLimit
                    Layout.fillWidth: true
                    validator: RegularExpressionValidator { regularExpression: /\d{0,6}/ }
                    Accessible.name: "Member limit"
                }
                RvComboBox {
                    id: slowMode
                    Layout.fillWidth: true
                    Accessible.name: "Slow mode"
                    options: [{ value: 0, label: "Off" }, { value: 10, label: "10 seconds" }, { value: 60, label: "1 minute" }, { value: 300, label: "5 minutes" }]
                    value: 0
                }
            }
            FieldLabel { text: "Rules" }
            PostField {
                id: rules
                Layout.fillWidth: true
                limit: 4000
                Accessible.name: "Rules"
            }
            FieldLabel { text: "Public preview" }
            PostField {
                id: preview
                Layout.fillWidth: true
                limit: 1000
                Accessible.name: "Public preview"
            }
            ConsentBox {
                id: paused
                Layout.fillWidth: true
                label: "Pause new memberships"
            }
            ConsentBox {
                id: invitesOff
                Layout.fillWidth: true
                label: "Suspend invite links"
            }
            ConsentBox {
                id: approval
                Layout.fillWidth: true
                visible: manage.loaded && manage.c.accessType === "free"
                label: "Approve each person who asks to join"
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                RvButton {
                    text: "Save settings"
                    enabled: !manage.communities.busy && name.text.trim().length > 0 && !description.tooLong && !rules.tooLong && !preview.tooLong
                    onClicked: manage.communities.saveSettings({
                        name: name.text,
                        description: description.text,
                        category: category.value,
                        rules: rules.text,
                        previewText: preview.text,
                        visibility: visibility.value,
                        memberLimit: memberLimit.text,
                        slowModeSeconds: slowMode.value,
                        joinApproval: approval.checked,
                        newMembersPaused: paused.checked,
                        invitationsSuspended: invitesOff.checked
                    })
                }
                RvButton {
                    secondary: true
                    iconName: "image"
                    text: "Picture…"
                    enabled: !manage.communities.busy
                    onClicked: {
                        imageDialog.kind = "avatar"
                        imageDialog.open()
                    }
                }
                RvButton {
                    secondary: true
                    iconName: "image"
                    text: "Banner…"
                    enabled: !manage.communities.busy
                    onClicked: {
                        imageDialog.kind = "banner"
                        imageDialog.open()
                    }
                }
            }
            FileDialog {
                id: imageDialog
                property string kind: "avatar"
                title: kind === "avatar" ? "Choose a picture (up to 300 KB)" : "Choose a banner (up to 800 KB)"
                nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
                onAccepted: manage.communities.setCommunityImage(kind, selectedFile)
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: manage.loaded && manage.c.accessType === "paid" && manage.role === "owner"
                spacing: 8
                Caption { text: "PRICE" }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.text3
                    font.pixelSize: 13
                    text: "A new price applies to new members only. Everyone already subscribed keeps the price they agreed to."
                          + (manage.communities.config.minPriceMinor !== undefined
                             ? " Allowed: " + manage.communities.formatMoney(manage.communities.config.minPriceMinor, manage.communities.config.currency)
                               + " to " + manage.communities.formatMoney(manage.communities.config.maxPriceMinor, manage.communities.config.currency) + " a month." : "")
                }
                RowLayout {
                    spacing: 8
                    RvTextField {
                        id: price
                        implicitWidth: 160
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        Accessible.name: "Monthly price in US dollars"
                    }
                    RvButton {
                        secondary: true
                        text: "Set new price"
                        enabled: !manage.communities.busy
                        onClicked: manage.communities.setPrice(price.text)
                    }
                }
            }
        }

        // --- Members ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: manage.section === "members"
            spacing: 8
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [["active", "Members"], ["pending_approval", "Requests"], ["pending_enrollment", "Waiting for key"], ["banned", "Banned"]]
                    delegate: Chip {
                        required property var modelData
                        text: modelData[1]
                        checked: manage.communities.memberFilter === modelData[0]
                        onClicked: manage.communities.loadMembers(modelData[0])
                    }
                }
            }
            Text {
                visible: manage.communities.members.length === 0
                text: "Nobody here."
                color: Theme.text3
            }
            Repeater {
                model: manage.communities.members
                delegate: Rectangle {
                    id: memberRow
                    required property var modelData
                    readonly property var m: modelData
                    readonly property string filter: manage.communities.memberFilter
                    readonly property bool outranked: m.role !== "owner" && manage.rank[manage.role] > manage.rank[m.role]
                    Layout.fillWidth: true
                    implicitHeight: Math.max(60, memberTexts.implicitHeight + 20, memberActions.implicitHeight + 16)
                    radius: Theme.radiusControl
                    color: Theme.surface
                    border.color: Theme.border
                    Column {
                        id: memberTexts
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.right: memberActions.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            width: parent.width
                            text: memberRow.m.username
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.weight: Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.Wrap
                            color: Theme.text3
                            font.pixelSize: Theme.fontSmall + 1
                            text: memberRow.m.role
                                  + (manage.loaded && manage.c.kind === "channel" && memberRow.m.role === "member" ? (memberRow.m.canPost ? " · can post" : " · reads only") : "")
                                  + (memberRow.m.entitlement ? " · billing: " + memberRow.m.entitlement.replace("_", " ") : "")
                                  + (memberRow.m.statusReason ? " · " + memberRow.m.statusReason : "")
                            textFormat: Text.PlainText
                        }
                    }
                    Row {
                        id: memberActions
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        layoutDirection: Qt.RightToLeft
                        RvButton {
                            visible: memberRow.filter === "pending_approval"
                            danger: true
                            primary: false
                            text: "Decline"
                            onClicked: manage.communities.decideRequest(memberRow.m.userId, false)
                        }
                        RvButton {
                            visible: memberRow.filter === "pending_approval"
                            secondary: true
                            text: "Approve"
                            onClicked: manage.communities.decideRequest(memberRow.m.userId, true)
                        }
                        RvButton {
                            visible: memberRow.filter === "banned" && manage.rank[manage.role] >= 2
                            secondary: true
                            text: "Unban"
                            onClicked: manage.communities.unban(memberRow.m.userId)
                        }
                        RvButton {
                            visible: memberRow.filter === "active" && memberRow.outranked
                            primary: false
                            danger: true
                            text: "Ban"
                            onClicked: {
                                manage.acting = memberRow.m
                                banDialog.open()
                            }
                        }
                        RvButton {
                            visible: memberRow.filter === "active" && memberRow.outranked
                            primary: false
                            danger: true
                            text: "Remove"
                            onClicked: {
                                manage.acting = memberRow.m
                                removeDialog.open()
                            }
                        }
                        RvButton {
                            visible: memberRow.filter === "active" && memberRow.outranked && manage.rank[manage.role] >= 2 && memberRow.m.role === "member"
                            secondary: true
                            text: memberRow.m.canPost ? "Mute" : "Allow posting"
                            onClicked: manage.communities.setMemberCanPost(memberRow.m.userId, !memberRow.m.canPost)
                        }
                        RvComboBox {
                            visible: memberRow.filter === "active" && memberRow.outranked && manage.rank[manage.role] >= 2
                            implicitWidth: 140
                            Accessible.name: "Role for " + memberRow.m.username
                            options: [{ value: "member", label: "Member" }, { value: "moderator", label: "Moderator" }]
                                     .concat(manage.role === "owner" ? [{ value: "admin", label: "Admin" }] : [])
                            value: memberRow.m.role
                            onPicked: (role) => manage.communities.setMemberRole(memberRow.m.userId, role)
                        }
                    }
                }
            }
        }

        // --- Invitations ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: manage.section === "invitations"
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: 13
                text: manage.loaded ? "An invite link lets someone see this community" + (manage.c.visibility === "unlisted" ? " (it's unlisted)" : "") + " and "
                                      + (manage.c.accessType === "paid" ? "subscribe" : "join") + ". It never skips payment." : ""
                textFormat: Text.PlainText
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                RvTextField {
                    id: maxUses
                    Layout.fillWidth: true
                    placeholderText: "Max uses (blank = unlimited)"
                    validator: RegularExpressionValidator { regularExpression: /\d{0,5}/ }
                    Accessible.name: "Max uses"
                }
                RvComboBox {
                    id: expires
                    Accessible.name: "Expires"
                    options: [{ value: 1, label: "Expires in 1 day" }, { value: 7, label: "Expires in 7 days" }, { value: 30, label: "Expires in 30 days" }, { value: 0, label: "Never expires" }]
                    value: 30
                }
                RvButton {
                    text: "Create link"
                    enabled: !manage.communities.busy
                    onClicked: manage.communities.createInvitation(maxUses.text.length > 0 ? parseInt(maxUses.text) : 0, expires.value)
                }
            }
            InfoCard {
                Layout.fillWidth: true
                visible: manage.communities.createdInvite.length > 0
                tone: "ok"
                text: "Copy this link now: it's shown only once."
                detail: manage.communities.createdInvite
                RvButton {
                    secondary: true
                    iconName: "copy"
                    text: "Copy"
                    onClicked: manage.communities.copyText(manage.communities.createdInvite)
                }
            }
            Text {
                visible: manage.communities.invitations.length === 0
                text: "No invite links."
                color: Theme.text3
            }
            Repeater {
                model: manage.communities.invitations
                delegate: Rectangle {
                    id: inviteRow
                    required property var modelData
                    readonly property var i: modelData
                    Layout.fillWidth: true
                    implicitHeight: 60
                    radius: Theme.radiusControl
                    color: Theme.surface
                    border.color: Theme.border
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 14
                        anchors.right: revoke.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: "Used " + inviteRow.i.useCount + (inviteRow.i.maxUses ? " of " + inviteRow.i.maxUses : "") + " time(s)"
                            textFormat: Text.PlainText
                            color: Theme.text
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            color: Theme.text3
                            font.pixelSize: Theme.fontSmall + 1
                            text: "Created " + manage.communities.formatDate(inviteRow.i.createdAt) + " · "
                                  + (inviteRow.i.revokedAt ? "revoked " + manage.communities.formatDate(inviteRow.i.revokedAt)
                                     : inviteRow.i.expiresAt ? "expires " + manage.communities.formatDate(inviteRow.i.expiresAt) : "no expiry")
                            textFormat: Text.PlainText
                        }
                    }
                    RvButton {
                        id: revoke
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !inviteRow.i.revokedAt
                        primary: false
                        danger: true
                        text: "Revoke"
                        onClicked: manage.communities.revokeInvitation(inviteRow.i.id)
                    }
                }
            }
        }

        // --- Reports ---
        ColumnLayout {
            Layout.fillWidth: true
            visible: manage.section === "reports"
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.text2
                font.pixelSize: 13
                text: "Reports from members. Who reported is never shown. Reports about child safety, non-consensual images or other illegal content go to RedVault, not here."
            }
            Text {
                visible: manage.communities.reports.length === 0
                text: "No reports."
                color: Theme.text3
            }
            Repeater {
                model: manage.communities.reports
                delegate: Card {
                    id: reportCard
                    required property var modelData
                    readonly property var r: modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Pill {
                            text: reportCard.r.status
                            tone: reportCard.r.status === "open" ? "warn" : "plain"
                        }
                        Pill { text: reportCard.r.category.replace("_", " ") }
                        Pill { text: reportCard.r.targetType }
                        Text {
                            height: 24
                            verticalAlignment: Text.AlignVCenter
                            text: manage.communities.formatDate(reportCard.r.createdAt)
                            textFormat: Text.PlainText
                            color: Theme.text3
                            font.pixelSize: 12
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: (reportCard.r.details || "").length > 0
                        text: reportCard.r.details || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.text
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        visible: !!reportCard.r.evidencePlaintext
                        implicitHeight: evidence.implicitHeight + 20
                        radius: Theme.radiusSmall
                        color: Theme.surface2
                        Text {
                            id: evidence
                            anchors.fill: parent
                            anchors.margins: 10
                            text: reportCard.r.evidencePlaintext || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Theme.text2
                            font.pixelSize: 13
                        }
                    }
                    Text {
                        visible: !!reportCard.r.evidencePurgedAt
                        text: "Evidence removed after the retention period."
                        color: Theme.text3
                        font.pixelSize: 12
                    }
                    Flow {
                        Layout.fillWidth: true
                        visible: reportCard.r.status === "open"
                        spacing: 8
                        RvButton {
                            secondary: true
                            text: "Mark handled"
                            onClicked: manage.communities.resolveReport(reportCard.r.id, "actioned")
                        }
                        RvButton {
                            primary: false
                            text: "Dismiss"
                            onClicked: manage.communities.resolveReport(reportCard.r.id, "dismissed")
                        }
                    }
                }
            }
        }
    }

    component Caption: Text {
        color: Theme.text3
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }
    component FieldLabel: Text {
        color: Theme.text2
        font.pixelSize: 13
    }
}
