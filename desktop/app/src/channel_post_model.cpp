#include "channel_post_model.hpp"

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QVariantList>

#include <algorithm>
#include <utility>

#include "chat_models.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/text_format.hpp"
#include "redvault/util/text.hpp"

namespace redvault::app {

namespace {

namespace channels = api::channels;
namespace posts = model::channel_posts;

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

/// The local day a post was published on (invalid when its time can't be read).
QDate dayOf(const std::string& iso) {
    const auto ms = util::parseIsoMillis(iso);
    return ms ? QDateTime::fromMSecsSinceEpoch(*ms).toLocalTime().date() : QDate();
}

/// "1 view", "1,234 views", as the web app's `${n.toLocaleString()} ${n === 1 ? "view" : "views"}`.
QString viewsLabel(std::int64_t n) {
    return QLocale().toString(static_cast<qlonglong>(n)) + (n == 1 ? QStringLiteral(" view") : QStringLiteral(" views"));
}

QVariantMap pollOf(const channels::Poll& poll, const std::optional<posts::RememberedVote>& mine) {
    const bool showResults = mine.has_value() || poll.closed;
    const std::optional<int> correct = poll.correct ? poll.correct : mine ? mine->correct : std::nullopt;
    const std::optional<std::string> explanation = poll.explanation ? poll.explanation : mine ? mine->explanation : std::nullopt;
    const auto percent = posts::percents(poll);
    QVariantList options;
    for (std::size_t i = 0; i < poll.options.size(); ++i) {
        const int index = static_cast<int>(i);
        const bool chosen = mine && std::find(mine->mine.begin(), mine->mine.end(), index) != mine->mine.end();
        const bool right = poll.quiz && correct == index;
        const bool wrong = poll.quiz && chosen && correct.has_value() && *correct != index;
        options.append(QVariantMap{{"label", qs(poll.options[i])},
                                   {"result", (chosen ? QStringLiteral("✓ ") : QString()) + qs(poll.options[i]) +
                                                  (right ? QStringLiteral(" (right answer)") : QString())},
                                   {"percent", percent[i]},
                                   {"chosen", chosen},
                                   {"right", right},
                                   {"wrong", wrong}});
    }
    QString kind = poll.quiz ? QStringLiteral("Quiz") : poll.multiple ? QStringLiteral("Poll · pick any") : QStringLiteral("Poll");
    if (poll.closed) kind += QStringLiteral(" · closed");
    QString votes = QString::number(poll.voters) + (poll.voters == 1 ? QStringLiteral(" vote") : QStringLiteral(" votes"));
    if (!showResults) votes += QStringLiteral(" · results show after you vote");
    return {{"question", qs(poll.question)},
            {"kindLabel", kind},
            {"multiple", poll.multiple},
            {"quiz", poll.quiz},
            {"closed", poll.closed},
            {"voters", poll.voters},
            {"votesLabel", votes},
            {"showResults", showResults},
            {"canTakeBack", mine.has_value() && !poll.quiz && !poll.closed},
            {"closeLabel", poll.quiz ? QStringLiteral("Close and show the answer") : QStringLiteral("Close poll")},
            {"explanation", poll.quiz && showResults && explanation ? qs(*explanation) : QString()},
            {"options", options}};
}

QVariantMap checklistOf(const channels::Checklist& checklist) {
    QVariantList items;
    int done = 0;
    for (const auto& item : checklist.items) {
        items.append(QVariantMap{{"text", qs(item.text)}, {"done", item.done}});
        if (item.done) ++done;
    }
    QString label = QStringLiteral("%1 of %2 done").arg(done).arg(checklist.items.size());
    if (checklist.collaborative) label += QStringLiteral(" · anyone following can tick items");
    return {{"title", qs(checklist.title)}, {"items", items}, {"doneLabel", label}, {"collaborative", checklist.collaborative}};
}

}  // namespace

ChannelPostModel::ChannelPostModel(VoteLookup votes, QObject* parent) : QAbstractListModel(parent), votes_(std::move(votes)) {}

int ChannelPostModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(posts_.size());
}

QVariant ChannelPostModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto& post = posts_[static_cast<std::size_t>(index.row())];
    if (role == IdRole) return qs(post.id);
    if (role == PostRole) return rowOf(post, post.poll && votes_ ? votes_(post.id) : std::nullopt);
    if (role == DayHeaderRole) return dayHeader(index.row());
    return {};
}

QHash<int, QByteArray> ChannelPostModel::roleNames() const {
    return {{PostRole, "post"}, {IdRole, "postId"}, {DayHeaderRole, "dayHeader"}};
}

QString ChannelPostModel::dayHeader(int row) const {
    const auto& post = posts_[static_cast<std::size_t>(row)];
    const auto ms = util::parseIsoMillis(post.createdAt);
    if (!ms) return {};
    const std::size_t older = static_cast<std::size_t>(row) + 1;
    if (older < posts_.size() && dayOf(posts_[older].createdAt) == dayOf(post.createdAt)) return {};
    return formatDay(*ms);
}

const channels::Post* ChannelPostModel::find(const std::string& id) const {
    const auto it = std::find_if(posts_.begin(), posts_.end(), [&](const channels::Post& p) { return p.id == id; });
    return it == posts_.end() ? nullptr : &*it;
}

void ChannelPostModel::reset(std::vector<channels::Post> list) {
    beginResetModel();
    posts_ = std::move(list);
    endResetModel();
    emit countChanged();
}

void ChannelPostModel::append(std::vector<channels::Post> list) {
    // Pages can overlap when something was posted meanwhile: each post shows once.
    std::erase_if(list, [&](const channels::Post& p) { return find(p.id) != nullptr; });
    if (list.empty()) return;
    const int firstNew = count();
    beginInsertRows({}, firstNew, firstNew + static_cast<int>(list.size()) - 1);
    posts_.insert(posts_.end(), std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()));
    endInsertRows();
    emit countChanged();
    // The post that was the oldest has one above it now: it may not start its day any more.
    if (firstNew > 0) {
        const auto at = index(firstNew - 1);
        emit dataChanged(at, at, {DayHeaderRole});
    }
}

void ChannelPostModel::prepend(channels::Post post) {
    if (find(post.id)) {
        replace(post);
        return;
    }
    beginInsertRows({}, 0, 0);
    posts_.insert(posts_.begin(), std::move(post));
    endInsertRows();
    emit countChanged();
}

void ChannelPostModel::replace(const channels::Post& post) {
    for (std::size_t i = 0; i < posts_.size(); ++i) {
        if (posts_[i].id != post.id) continue;
        posts_[i] = post;
        const auto at = index(static_cast<int>(i));
        emit dataChanged(at, at, {PostRole});
        return;
    }
}

void ChannelPostModel::remove(const std::string& id) {
    for (std::size_t i = 0; i < posts_.size(); ++i) {
        if (posts_[i].id != id) continue;
        beginRemoveRows({}, static_cast<int>(i), static_cast<int>(i));
        posts_.erase(posts_.begin() + static_cast<std::ptrdiff_t>(i));
        endRemoveRows();
        emit countChanged();
        // The post below it may start the day now.
        if (i > 0) {
            const auto at = index(static_cast<int>(i) - 1);
            emit dataChanged(at, at, {DayHeaderRole});
        }
        return;
    }
}

void ChannelPostModel::refresh(const std::string& id) {
    if (const auto* post = find(id)) replace(*post);
}

QString ChannelPostModel::when(const std::string& iso) {
    const auto ms = util::parseIsoMillis(iso);
    if (!ms) return {};
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(*ms).toLocalTime();
    const QLocale locale;
    return locale.toString(at.date(), QStringLiteral("d MMM yyyy")) + QStringLiteral(", ") + locale.toString(at.time(), QLocale::ShortFormat);
}

QVariantMap ChannelPostModel::mediaOf(const channels::Media& m) {
    const QString name = m.filename ? qs(*m.filename) : qs(posts::kindLabel(m.kind));
    return {{"id", qs(m.id)},
            {"kind", qs(m.kind)},
            {"url", qs(m.url)},
            {"mime", qs(m.mime)},
            {"size", static_cast<qlonglong>(m.size)},
            {"name", name},
            {"hasName", m.filename.has_value()},
            {"sizeLabel", qs(model::file_message::formatBytes(m.size))},
            {"durationMs", static_cast<qlonglong>(m.durationMs.value_or(0))},
            {"durationLabel", m.durationMs ? qs(posts::formatDuration(static_cast<double>(*m.durationMs) / 1000.0)) : QString()},
            {"width", m.width.value_or(0)},
            {"height", m.height.value_or(0)}};
}

QVariantMap ChannelPostModel::rowOf(const channels::Post& post, const std::optional<posts::RememberedVote>& mine) {
    QVariantList photos;
    QVariantList others;
    for (const auto& m : post.media) (m.kind == "photo" ? photos : others).append(mediaOf(m));
    model::text_format::Options hidden;
    model::text_format::Options shown;
    shown.revealSpoilers = true;
    const auto ms = util::parseIsoMillis(post.createdAt);
    return {{"id", qs(post.id)},
            {"when", when(post.createdAt)},
            {"time", ms ? formatClock(*ms) : QString()},
            {"views", viewsLabel(post.views)},
            {"suggested", post.suggested},
            {"body", qs(post.body)},
            {"bodyRich", qs(model::text_format::toHtml(post.body, hidden))},
            {"bodyRevealed", qs(model::text_format::toHtml(post.body, shown))},
            {"photos", photos},
            {"others", others},
            {"poll", post.poll ? pollOf(*post.poll, mine) : QVariantMap()},
            {"checklist", post.checklist ? checklistOf(*post.checklist) : QVariantMap()}};
}

}  // namespace redvault::app
