#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Interactive and informational story stickers, as the web app's
// ui/src/lib/storyCrypto.ts parses them (docs/stories-advanced/): the same JSON,
// limits and fallbacks, pinned by tests/story_interactive_test.cpp against
// vectors the web code made. Everything here comes out of a decrypted story
// someone else wrote: untrusted input. A sticker that doesn't fit is dropped on
// its own, and a bad one never hides the story.

namespace redvault::model::stories {

class StoryFormatError : public std::runtime_error {
public:
    StoryFormatError() : std::runtime_error("Malformed story") {}
};

inline constexpr std::size_t kMaxStickers = 12;
inline constexpr std::size_t kMaxStickerText = 120;    // UTF-16 units
inline constexpr std::size_t kMaxStickerOption = 40;   // UTF-16 units
inline constexpr std::size_t kMaxQuestionAnswer = 500;  // UTF-16 units
inline constexpr std::size_t kMaxLinkLength = 300;
inline constexpr std::size_t kMaxStickerOptions = 4;
/// A story with something to answer stays up this long in the viewer (a photo or text story: kStorySeconds).
inline constexpr int kStickerStorySeconds = 20;

/// One sticker. `type` is "poll", "quiz", "question", "slider", "countdown", "link", "mention", "date" or "location";
/// only the fields of that type are filled.
struct Sticker {
    std::string type;
    /// 6-16 characters of a-z0-9; the server only ever sees this id.
    std::string id;
    /// Centre, as fractions of the frame.
    double x = 0.5;
    double y = 0.5;
    double scale = 1;     // 0.6 - 2.5
    double rotation = 0;  // degrees, 0 - 360

    /// poll/quiz: the question; question/slider: the prompt; countdown: the title; link/location: the label.
    std::string text;
    /// poll/quiz: two to four options.
    std::vector<std::string> options;
    /// quiz: index into options.
    int correct = 0;
    std::string explanation;
    /// poll/quiz/slider: whether everyone who can see the story sees the totals ("everyone") or only the author ("author").
    bool resultsEveryone = true;
    /// poll: epoch ms, voting stops then (absent: until the story ends).
    std::optional<double> closesAt;
    /// question: answers arrive without the sender's name.
    bool anonymous = false;
    /// slider.
    std::string emoji;
    /// countdown: endsAt; date: at (epoch ms).
    double when = 0;
    /// date: "date", "time" or "datetime".
    std::string style;
    /// link: an https address.
    std::string url;
    /// mention.
    std::string userId;
};

bool isAnswerable(const Sticker& sticker);
/// Answers to a question sticker marked "without my name" carry no sender.
bool isAnonymousSticker(const Sticker& sticker);
/// Quiz and question answers are final: a second one is refused by the server. Polls and sliders can be changed.
bool isFinalSticker(const Sticker& sticker);
/// Whether a poll's voting deadline has passed.
bool isClosed(const Sticker& sticker, std::int64_t nowMs);

/// Control characters and bidi overrides out, whitespace tidied; empty or longer than `maxUnits` UTF-16 units gives nullopt.
std::optional<std::string> cleanStickerText(std::string_view utf8, std::size_t maxUnits);

/**
 * A link a story may carry: https only, no credentials, a real host. Returns it the way the web app's URL
 * parser spells it (lowercase host, "/" for an empty path, default port left out). Stricter than the web:
 * anything that would need rewriting beyond that (international host names, spaces, backslashes, ".." paths,
 * unusual characters) is refused rather than guessed at.
 */
std::optional<std::string> safeStoryLink(std::string_view raw);

/// `jsonArray` is the JSON text of the stickers; anything malformed or unknown is dropped, as is a repeated id and everything after the twelfth.
std::vector<Sticker> parseStickers(std::string_view jsonArray);
/// The JSON the web app writes for one sticker / a list of them.
std::string toJson(const Sticker& sticker);
std::string toJson(const std::vector<Sticker>& stickers);

// --- Answers and totals ------------------------------------------------------------------------

/// What a viewer sent for one sticker.
struct Answer {
    std::string kind;  // "poll" | "quiz" | "slider" | "question"
    int choice = 0;    // poll, quiz
    int value = 0;     // slider, 0-100
    std::string text;  // question, trimmed, at most kMaxQuestionAnswer units
};

/// Checks an answer against the sticker it is for: anything that doesn't fit throws StoryFormatError, never half-counted.
Answer parseAnswer(std::string_view json, const Sticker& sticker);
Answer checkedAnswer(const Answer& answer, const Sticker& sticker);
std::string toJson(const Answer& answer);

/// Totals the author published for a poll, quiz or slider.
struct Totals {
    int total = 0;
    /// poll, quiz: answers per option.
    std::vector<int> counts;
    /// quiz: how many chose the right option.
    std::optional<int> correct;
    /// slider: the mean position, 0-100, to a tenth.
    std::optional<double> average;
};

Totals parseTotals(std::string_view json, const Sticker& sticker);
std::string toJson(const Totals& totals);

/// One answer the author opened.
struct InboxEntry {
    std::string stickerId;
    /// Empty for an answer sent without a name.
    std::string responderId;
    Answer answer;
    /// Epoch ms, to the minute.
    std::int64_t atMs = 0;
    int revision = 0;
    /// The sealed envelope as stored (base64): how an answer is pointed at when reporting it.
    std::string envelope;
};

/**
 * The totals for one sticker. A poll ignores answers sent after its deadline (to the minute: the server
 * keeps times only that precisely); a quiz counts only a first answer.
 */
Totals tally(const Sticker& sticker, const std::vector<InboxEntry>& entries);

}  // namespace redvault::model::stories
