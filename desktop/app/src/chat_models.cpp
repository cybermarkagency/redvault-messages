// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "chat_models.hpp"

#include <QDateTime>
#include <QLocale>

#include <algorithm>

#include "redvault/api/community_api.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/text_format.hpp"

namespace redvault::app {

namespace {

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

QDateTime local(std::int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime();
}

QString previewOf(const ChatSummary& chat) {
    if (chat.messages.empty()) return QStringLiteral("No messages yet");
    const auto& last = chat.messages.back();
    QString text;
    switch (last.kind) {
        case data::MessageKind::File:
            text = QStringLiteral("\U0001F4CE ") + qs(last.text);  // paperclip
            break;
        case data::MessageKind::Undecryptable:
            text = QStringLiteral("Couldn't decrypt this message");
            break;
        case data::MessageKind::Poll:
            text = QStringLiteral("\U0001F4CA ") + qs(last.text);  // bar chart
            break;
        case data::MessageKind::Notice:
            text = last.notice == "pin" ? QStringLiteral("pinned \u201C%1\u201D").arg(qs(model::text_format::toPlain(last.text)).simplified())
                                        : QStringLiteral("unpinned a message");
            if (last.fromMe) return QStringLiteral("You ") + text;
            return qs(chat.meta->handleOf(last.senderId).empty() ? chat.meta->handle : chat.meta->handleOf(last.senderId)) +
                   u' ' + text;
        default:
            // Formatting markers dropped and spoilers hidden, like the notification would.
            text = qs(model::text_format::toPlain(last.text)).simplified();
    }
    if (last.fromMe) return QStringLiteral("You: ") + text;
    if (chat.meta->isGroup) return qs(chat.meta->handleOf(last.senderId)) + QStringLiteral(": ") + text;
    return text;
}

/// Everything a row shows, so an unchanged message can be told apart from a changed one.
QString signature(const data::Message& m) {
    QString out = qs(m.id) + qs(m.text) + QString::number(int(m.kind)) + (m.seen ? u'S' : u's') +
                  (m.delivered ? u'D' : u'd') + qs(m.fileStatus.value_or(""));
    for (const auto& reaction : m.reactions) out += qs(reaction.emoji) + QString::number(reaction.count) + (reaction.mine ? u'M' : u'm');
    if (m.security) out += QString::number(int(m.security->verification));
    if (m.editedAtMs) out += QString::number(*m.editedAtMs);
    if (m.forwarded) out += u'F';
    if (m.unverifiedSender) out += u'U';
    if (m.channelPost) out += qs(m.channelPost->postId) + qs(m.channelPost->channelName);
    if (m.storyReply) out += u'R';
    if (m.poll) {
        out += m.poll->closed ? u'C' : u'c';
        for (const int count : m.poll->counts) out += QString::number(count) + u',';
        for (const int mine : m.poll->mine) out += QString::number(mine) + u';';
    }
    return out;
}

}  // namespace

QString communityAvatarPath(const data::ConversationMeta& meta) {
    if (!meta.community || !meta.community->hasAvatar) return {};
    // A member sees it without an invitation.
    return qs(api::community::imagePath(meta.community->id, "avatar"));
}

bool isInlineImage(const data::Message& m) {
    if (m.kind != data::MessageKind::File || !m.file || m.fileStatus.value_or("available") != "available") return false;
    const QString mime = qs(m.file->mimeType).toLower();
    // The decoder checks the bytes themselves; this only picks what to try.
    return mime == QStringLiteral("image/jpeg") || mime == QStringLiteral("image/png") || mime == QStringLiteral("image/gif") ||
           mime == QStringLiteral("image/webp") || mime == QStringLiteral("image/bmp");
}

QString formatListTime(std::int64_t ms) {
    if (ms <= 0) return {};
    const QDateTime when = local(ms);
    const QDate today = QDate::currentDate();
    const QLocale locale;
    if (when.date() == today) return locale.toString(when.time(), QLocale::ShortFormat);
    if (when.date().daysTo(today) < 7) return locale.toString(when.date(), QStringLiteral("ddd"));
    return locale.toString(when.date(), QStringLiteral("MMM d"));
}

QString formatClock(std::int64_t ms) {
    return QLocale().toString(local(ms).time(), QLocale::ShortFormat);
}

QString formatDay(std::int64_t ms) {
    const QDate date = local(ms).date();
    const QDate today = QDate::currentDate();
    if (date == today) return QStringLiteral("Today");
    if (date.daysTo(today) == 1) return QStringLiteral("Yesterday");
    return QLocale().toString(date, date.year() == today.year() ? QStringLiteral("dddd, MMMM d")
                                                                 : QStringLiteral("MMMM d, yyyy"));
}

// --- ChatListModel ---

int ChatListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(chats_.size());
}

QVariant ChatListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto& chat = chats_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case ChatIdRole:
            return qs(chat.meta->id);
        case TitleRole:
            return qs(chat.meta->handle);
        case InitialRole: {
            const QString title = qs(chat.meta->handle).trimmed();
            return title.isEmpty() ? QStringLiteral("?") : title.left(1).toUpper();
        }
        case IsGroupRole:
            return chat.meta->isGroup;
        case PreviewRole:
            return previewOf(chat);
        case TimeRole:
            return formatListTime(chat.lastActivityMs());
        case UnreadRole:
            return chat.unread;
        case AvatarUrlRole:
            return chat.meta->isGroup ? QString() : qs(chat.meta->avatarUrl.value_or(""));
        case UserIdRole:
            return chat.meta->isGroup ? QString() : qs(chat.meta->otherUserId);
        case DraftRole:
            return drafts_.value(qs(chat.meta->id)).simplified();
        case CommunityImageRole:
            return communityAvatarPath(*chat.meta);
        default:
            return {};
    }
}

QHash<int, QByteArray> ChatListModel::roleNames() const {
    return {{ChatIdRole, "chatId"},   {TitleRole, "title"},     {InitialRole, "initial"}, {IsGroupRole, "isGroup"},
            {PreviewRole, "preview"}, {TimeRole, "timeText"},  {UnreadRole, "unread"},
            {AvatarUrlRole, "avatarUrl"}, {UserIdRole, "userId"}, {DraftRole, "draft"},
            {CommunityImageRole, "communityImage"}};
}

void ChatListModel::setChats(std::vector<ChatSummary> chats) {
    std::stable_sort(chats.begin(), chats.end(),
                     [](const ChatSummary& a, const ChatSummary& b) { return a.lastActivityMs() > b.lastActivityMs(); });
    const bool sameRows = chats.size() == chats_.size() &&
                          std::equal(chats.begin(), chats.end(), chats_.begin(), [](const auto& a, const auto& b) {
                              return a.meta->id == b.meta->id;
                          });
    if (sameRows) {
        chats_ = std::move(chats);
        if (!chats_.empty()) emit dataChanged(index(0), index(rowCount() - 1));
        return;
    }
    beginResetModel();
    chats_ = std::move(chats);
    endResetModel();
    emit countChanged();
}

void ChatListModel::setDraft(const QString& chatId, const QString& text) {
    const QString trimmed = text.trimmed().isEmpty() ? QString() : text;
    if (drafts_.value(chatId) == trimmed) return;
    if (trimmed.isEmpty()) drafts_.remove(chatId);
    else drafts_.insert(chatId, trimmed);
    for (std::size_t i = 0; i < chats_.size(); ++i) {
        if (qs(chats_[i].meta->id) == chatId) emit dataChanged(index(static_cast<int>(i)), index(static_cast<int>(i)), {DraftRole});
    }
}

void ChatListModel::clear() {
    setChats({});
}

const ChatSummary* ChatListModel::find(const std::string& id) const {
    const auto it = std::find_if(chats_.begin(), chats_.end(), [&](const ChatSummary& c) { return c.meta->id == id; });
    return it == chats_.end() ? nullptr : &*it;
}

ChatSummary* ChatListModel::find(const std::string& id) {
    const auto it = std::find_if(chats_.begin(), chats_.end(), [&](const ChatSummary& c) { return c.meta->id == id; });
    return it == chats_.end() ? nullptr : &*it;
}

// --- MessageModel ---

int MessageModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(messages_.size());
}

QVariant MessageModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const auto row = static_cast<std::size_t>(index.row());
    const auto& m = messages_[row];
    const data::Message* previous = row > 0 ? &messages_[row - 1] : nullptr;
    switch (role) {
        case MessageIdRole:
            return qs(m.id);
        case TextRole:
            return qs(m.text);
        case FromMeRole:
            return m.fromMe;
        case SenderRole:
            if (m.fromMe) return QStringLiteral("You");
            return meta_ && meta_->isGroup ? qs(meta_->handleOf(m.senderId)) : (meta_ ? qs(meta_->handle) : QString());
        case ShowSenderRole:
            // Groups only: name the sender at the start of each run of their messages.
            return meta_ && meta_->isGroup && !m.fromMe &&
                   (previous == nullptr || previous->senderId != m.senderId || previous->fromMe ||
                    local(previous->createdAtMs).date() != local(m.createdAtMs).date());
        case TimeRole:
            return formatClock(m.createdAtMs);
        case DayHeaderRole:
            if (previous != nullptr && local(previous->createdAtMs).date() == local(m.createdAtMs).date()) return QString();
            return formatDay(m.createdAtMs);
        case KindRole:
            switch (m.kind) {
                case data::MessageKind::File:
                    return QStringLiteral("file");
                case data::MessageKind::Undecryptable:
                    return QStringLiteral("undecryptable");
                case data::MessageKind::Notice:
                    return QStringLiteral("notice");
                case data::MessageKind::Poll:
                    return QStringLiteral("poll");
                default:
                    return QStringLiteral("text");
            }
        case FileSizeRole:
            return m.file ? qs(model::file_message::formatBytes(m.file->originalSize)) : QString();
        case FileStatusRole:
            return qs(m.fileStatus.value_or(""));
        case StatusRole:
            if (!m.fromMe) return QString();
            // Still in the Tor outbox: never shown as sent before the server has it.
            if (m.queued) return QStringLiteral("queued");
            return m.seen ? QStringLiteral("seen") : m.delivered ? QStringLiteral("delivered") : QStringLiteral("sent");
        case QueueErrorRole:
            return qs(m.queueError);
        case QueueHeldRole:
            return m.queueHeld;
        case ReplyPreviewRole:
            return m.replyTo ? qs(m.replyTo->preview) : QString();
        case ReplyFromMeRole:
            return m.replyTo && m.replyTo->fromMe;
        case ReactionsRole: {
            QVariantList out;
            for (const auto& reaction : m.reactions) {
                out.append(QVariantMap{{QStringLiteral("emoji"), qs(reaction.emoji)},
                                       {QStringLiteral("count"), reaction.count},
                                       {QStringLiteral("mine"), reaction.mine}});
            }
            return out;
        }
        case RichTextRole:
        case RichTextRevealedRole: {
            if (m.kind != data::MessageKind::Text) return QString();
            model::text_format::Options options;
            options.revealSpoilers = role == RichTextRevealedRole;
            // In a group, "@handle" of a member is a mention (yours stands out).
            if (meta_ && meta_->isGroup) {
                for (const auto& member : meta_->members) {
                    options.mentions.push_back(member.handle);
                    if (member.isSelf) options.selfHandle = member.handle;
                }
            }
            return qs(model::text_format::toHtml(m.text, options));
        }
        case IsImageRole:
            return isInlineImage(m);
        case PollRole: {
            if (!m.poll) return QVariantMap();
            QVariantList options;
            for (std::size_t i = 0; i < m.poll->options.size(); ++i) {
                const bool mine = std::find(m.poll->mine.begin(), m.poll->mine.end(), static_cast<int>(i)) != m.poll->mine.end();
                options.append(QVariantMap{{"label", qs(m.poll->options[i])},
                                           {"count", i < m.poll->counts.size() ? m.poll->counts[i] : 0},
                                           {"mine", mine}});
            }
            return QVariantMap{{"question", qs(m.poll->question)}, {"multiple", m.poll->multiple},
                               {"closed", m.poll->closed},     {"voters", m.poll->voters},
                               {"voted", !m.poll->mine.empty()}, {"options", options}};
        }
        case NoticeTextRole: {
            if (m.kind != data::MessageKind::Notice) return QString();
            QString who = m.fromMe ? QStringLiteral("You") : meta_ ? qs(meta_->isGroup ? meta_->handleOf(m.senderId) : meta_->handle) : QString();
            if (who.isEmpty()) who = QStringLiteral("Someone");
            return m.notice == "pin" ? QStringLiteral("%1 pinned \u201C%2\u201D").arg(who, qs(model::text_format::toPlain(m.text)))
                                     : QStringLiteral("%1 unpinned a message").arg(who);
        }
        case EditedRole:
            return m.editedAtMs.has_value();
        case ForwardedRole:
            return m.forwarded;
        case UnverifiedSenderRole:
            return m.unverifiedSender;
        case ChannelIdRole:
            return m.channelPost ? qs(m.channelPost->channelId) : QString();
        case ChannelNameRole:
            return m.channelPost ? qs(m.channelPost->channelName) : QString();
        case ChannelPostIdRole:
            return m.channelPost ? qs(m.channelPost->postId) : QString();
        case StoryReplyRole: {
            if (!m.storyReply) return QString();
            const auto& r = *m.storyReply;
            const QString what = m.fromMe ? (r.reaction ? QStringLiteral("You reacted to their story") : QStringLiteral("You replied to their story"))
                                          : (r.reaction ? QStringLiteral("Reacted to your story") : QStringLiteral("Replied to your story"));
            return what + QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ") + qs(r.preview);
        }
        case VerificationRole:
            if (!m.security) return QString();
            switch (m.security->verification) {
                case data::VerificationStatus::Verified:
                    return QStringLiteral("verified");
                case data::VerificationStatus::Failed:
                    return QStringLiteral("failed");
                default:
                    return QString();
            }
        default:
            return {};
    }
}

QHash<int, QByteArray> MessageModel::roleNames() const {
    return {{MessageIdRole, "messageId"},       {TextRole, "text"},
            {FromMeRole, "fromMe"},             {SenderRole, "sender"},
            {ShowSenderRole, "showSender"},     {TimeRole, "timeText"},
            {DayHeaderRole, "dayHeader"},       {KindRole, "kind"},
            {FileSizeRole, "fileSize"},         {FileStatusRole, "fileStatus"},
            {StatusRole, "status"},             {ReplyPreviewRole, "replyPreview"},
            {QueueErrorRole, "queueError"},     {QueueHeldRole, "queueHeld"},
            {ReplyFromMeRole, "replyFromMe"},   {ReactionsRole, "reactions"},
            {VerificationRole, "verification"}, {EditedRole, "edited"},
            {ForwardedRole, "forwarded"},       {UnverifiedSenderRole, "unverifiedSender"},       {StoryReplyRole, "storyReply"},       {RichTextRole, "richText"},
            {RichTextRevealedRole, "richTextRevealed"}, {IsImageRole, "isImage"},
            {PollRole, "poll"},                 {NoticeTextRole, "noticeText"},
            {ChannelIdRole, "channelId"},       {ChannelNameRole, "channelName"},
            {ChannelPostIdRole, "channelPostId"}};
}

void MessageModel::setMessages(std::shared_ptr<const data::ConversationMeta> meta, std::vector<data::Message> messages) {
    if (!meta_ || !meta || meta_->id != meta->id) {
        beginResetModel();
        meta_ = std::move(meta);
        messages_ = std::move(messages);
        endResetModel();
        emit countChanged();
        return;
    }
    meta_ = std::move(meta);
    const auto oldSize = messages_.size();

    // Older history arrived above, or the oldest rows slid out of the window:
    // change only the top, so the rows below (and the scroll position) stay put.
    std::size_t same = 0;
    if (!messages_.empty() && !messages.empty() && messages.front().id != messages_.front().id) {
        const auto byId = [](std::string id) { return [id = std::move(id)](const data::Message& m) { return m.id == id; }; };
        if (const auto oldFirst = std::find_if(messages.begin(), messages.end(), byId(messages_.front().id));
            oldFirst != messages.end()) {
            const auto lead = static_cast<int>(oldFirst - messages.begin());
            beginInsertRows({}, 0, lead - 1);
            messages_.insert(messages_.begin(), messages.begin(), oldFirst);
            endInsertRows();
            // The row that was first now has one above it: its day header may go.
            emit dataChanged(index(lead), index(lead));
            same = static_cast<std::size_t>(lead);
        } else if (const auto newFirst = std::find_if(messages_.begin(), messages_.end(), byId(messages.front().id));
                   newFirst != messages_.end()) {
            beginRemoveRows({}, 0, static_cast<int>(newFirst - messages_.begin()) - 1);
            messages_.erase(messages_.begin(), newFirst);
            endRemoveRows();
            emit dataChanged(index(0), index(0));
        }
    }

    // Keep the unchanged prefix; replace everything after it.
    while (same < messages_.size() && same < messages.size() && signature(messages_[same]) == signature(messages[same])) {
        ++same;
    }
    if (same < messages_.size()) {
        beginRemoveRows({}, static_cast<int>(same), static_cast<int>(messages_.size()) - 1);
        messages_.erase(messages_.begin() + static_cast<std::ptrdiff_t>(same), messages_.end());
        endRemoveRows();
    }
    if (same < messages.size()) {
        beginInsertRows({}, static_cast<int>(same), static_cast<int>(messages.size()) - 1);
        messages_.insert(messages_.end(), std::make_move_iterator(messages.begin() + static_cast<std::ptrdiff_t>(same)),
                         std::make_move_iterator(messages.end()));
        endInsertRows();
    }
    if (messages_.size() != oldSize) emit countChanged();
}

void MessageModel::remove(const std::string& id) {
    const auto it = std::find_if(messages_.begin(), messages_.end(), [&](const data::Message& m) { return m.id == id; });
    if (it == messages_.end()) return;
    const int row = static_cast<int>(it - messages_.begin());
    beginRemoveRows({}, row, row);
    messages_.erase(it);
    endRemoveRows();
    // The next message may now start a new day or a new run of one sender's messages.
    if (row < rowCount()) emit dataChanged(index(row), index(row));
    emit countChanged();
}

void MessageModel::clear() {
    beginResetModel();
    meta_.reset();
    messages_.clear();
    endResetModel();
    emit countChanged();
}

QVariantList MessageModel::files() const {
    QVariantList out;
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->kind != data::MessageKind::File || !it->file) continue;
        const QString mime = qs(it->file->mimeType);
        out.append(QVariantMap{
            {"messageId", qs(it->id)},
            {"name", qs(it->text)},
            {"size", qs(model::file_message::formatBytes(it->file->originalSize))},
            {"day", formatDay(it->createdAtMs)},
            {"fromMe", it->fromMe},
            {"image", mime.startsWith(QStringLiteral("image/"))},
            {"video", mime.startsWith(QStringLiteral("video/"))},
        });
    }
    return out;
}

int MessageModel::indexOf(const QString& messageId) const {
    const std::string id = messageId.toStdString();
    const auto it = std::find_if(messages_.begin(), messages_.end(), [&](const data::Message& m) { return m.id == id; });
    return it == messages_.end() ? -1 : static_cast<int>(it - messages_.begin());
}

QString MessageModel::idAt(int row) const {
    return row >= 0 && row < rowCount() ? qs(messages_[static_cast<std::size_t>(row)].id) : QString();
}

QStringList MessageModel::search(const QString& query) const {
    QStringList out;
    const QString needle = query.trimmed();
    if (needle.isEmpty()) return out;
    for (const auto& m : messages_) {
        if (m.kind == data::MessageKind::Undecryptable) continue;
        if (qs(m.text).contains(needle, Qt::CaseInsensitive)) out.append(qs(m.id));
    }
    return out;
}

const data::Message* MessageModel::find(const std::string& id) const {
    const auto it = std::find_if(messages_.begin(), messages_.end(), [&](const data::Message& m) { return m.id == id; });
    return it == messages_.end() ? nullptr : &*it;
}

}  // namespace redvault::app
