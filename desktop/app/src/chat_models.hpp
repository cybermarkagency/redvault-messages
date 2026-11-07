#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "redvault/data/chat_models.hpp"

namespace redvault::app {

/// One row of the chat list: the chat's (shared, read-only) meta plus its latest decrypted messages.
struct ChatSummary {
    std::shared_ptr<const data::ConversationMeta> meta;
    std::vector<data::Message> messages;  // oldest first
    int unread = 0;
    std::optional<int> disappearingSeconds;
    /// The server has older messages than `messages`.
    bool hasOlder = false;
    /// Pinned messages, oldest pin first.
    std::vector<data::PinnedView> pinned;

    std::int64_t lastActivityMs() const { return messages.empty() ? 0 : messages.back().createdAtMs; }
};

/// A community chat's picture path (for the "community" image provider), or empty: not a community, or it has none.
QString communityAvatarPath(const data::ConversationMeta& meta);

/// A picture the chat shows inline (by its declared type; the decoder checks the bytes).
bool isInlineImage(const data::Message& m);

/// "09:24" today, "Wed" this week, "Sep 24" before that. Local time.
QString formatListTime(std::int64_t ms);
/// "09:24", local time.
QString formatClock(std::int64_t ms);
/// "Today", "Yesterday", "Wednesday, September 24".
QString formatDay(std::int64_t ms);

class ChatListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ChatSession")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        ChatIdRole = Qt::UserRole + 1,
        TitleRole,
        InitialRole,
        IsGroupRole,
        PreviewRole,
        TimeRole,
        UnreadRole,
        AvatarUrlRole,
        UserIdRole,
        /// What's typed in the composer but not sent (memory only), or empty.
        DraftRole,
        /// A community's picture (communityAvatarPath), or empty.
        CommunityImageRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Replaces the rows, sorted by most recent activity; keeps views steady when only content changed.
    void setChats(std::vector<ChatSummary> chats);
    void clear();

    const ChatSummary* find(const std::string& id) const;
    ChatSummary* find(const std::string& id);
    std::vector<ChatSummary> snapshot() const { return chats_; }

    /// Unsent text per chat, kept in memory only (like everything else, gone when you quit).
    void setDraft(const QString& chatId, const QString& text);
    QString draft(const QString& chatId) const { return drafts_.value(chatId); }
    const std::vector<ChatSummary>& chats() const { return chats_; }

signals:
    void countChanged();

private:
    std::vector<ChatSummary> chats_;
    QHash<QString, QString> drafts_;
};

class MessageModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ChatSession")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        MessageIdRole = Qt::UserRole + 1,
        TextRole,
        FromMeRole,
        SenderRole,
        ShowSenderRole,
        TimeRole,
        DayHeaderRole,
        KindRole,
        FileSizeRole,
        FileStatusRole,
        StatusRole,
        QueueErrorRole,
        QueueHeldRole,
        ReplyPreviewRole,
        ReplyFromMeRole,
        ReactionsRole,
        VerificationRole,
        EditedRole,
        ForwardedRole,
        UnverifiedSenderRole,
        /// The text as rich text (bold, links, spoilers...), or empty when it's plain.
        RichTextRole,
        /// The same with spoilers shown.
        RichTextRevealedRole,
        /// A file the app can show inline (a JPEG, PNG, GIF, WebP or BMP picture).
        IsImageRole,
        /// MessageKind::Poll: {question, multiple, closed, voters, mine (bool), options: [{label, count, mine}]}.
        PollRole,
        /// MessageKind::Notice: the line to show ("bob pinned “…”").
        NoticeTextRole,
        /// Forwarded from a channel: the channel's id and name, and the post's id (all empty otherwise).
        ChannelIdRole,
        ChannelNameRole,
        ChannelPostIdRole,
        /// A reply or reaction to a story: "Replied to your story · Photo", or empty.
        StoryReplyRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /**
     * Shows `messages` (oldest first) for `meta`. Rows that didn't change
     * stay put, and new ones are appended, so a poll doesn't reset scrolling.
     */
    void setMessages(std::shared_ptr<const data::ConversationMeta> meta, std::vector<data::Message> messages);
    void clear();
    /// Removes one row (a deleted message) without disturbing the others.
    void remove(const std::string& id);

    const data::Message* find(const std::string& id) const;
    const std::vector<data::Message>& all() const { return messages_; }
    /// The row showing a message, or -1.
    Q_INVOKABLE int indexOf(const QString& messageId) const;
    /// The message on a row, or empty.
    Q_INVOKABLE QString idAt(int row) const;
    /// Ids of the messages whose text (or file name) contains `query`, ignoring case; oldest first.
    Q_INVOKABLE QStringList search(const QString& query) const;
    /// The files in this chat, newest first, for the shared-media list.
    QVariantList files() const;

signals:
    void countChanged();

private:
    std::shared_ptr<const data::ConversationMeta> meta_;
    std::vector<data::Message> messages_;
};

}  // namespace redvault::app
