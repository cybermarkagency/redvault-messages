#pragma once

#include <string>
#include <string_view>
#include <vector>

// Message formatting, WhatsApp-style markers inside the ordinary text, so
// apps that don't render it still show something readable:
//
//   *bold*   _italic_   ~strikethrough~   `monospace`   ||spoiler||
//
// plus http(s) links made clickable, and @handle mentions of a group's
// members highlighted (yours stands out more). A marker only opens after the start of
// a line, a space or an opening bracket/quote and only closes before the
// end, a space or punctuation, so snake_case, 2*3*4 and file_names stay as
// typed. Nothing inside `monospace` is formatted. Markers never span lines.
//
// The text is untrusted: everything is HTML-escaped first and only the tags
// produced here (b, i, s, span, a) ever appear in the output.

namespace redvault::model::text_format {

struct Options {
    /// CSS colour for links, e.g. "#86D9AE".
    std::string linkColor = "#86D9AE";
    /// Monospace font family name.
    std::string monoFamily = "IBM Plex Mono";
    /// Hidden spoilers are painted in this colour on this background (text unreadable until revealed).
    std::string spoilerColor = "#3A443F";
    /// Show spoilers' text (after the reader clicked one).
    bool revealSpoilers = false;
    /// Handles that "@handle" highlights (a group's members), matched ignoring ASCII case, longest first.
    std::vector<std::string> mentions;
    /// Your own handle: a mention of you is marked more strongly.
    std::string selfHandle;
    std::string mentionColor = "#86D9AE";
    std::string selfMentionBackground = "#244536";
};

/// Whether `text` mentions `handle` (as "@handle", the same rule toHtml() uses).
bool mentions(std::string_view text, std::string_view handle);

/**
 * Rich text (Qt's HTML subset) for a message, or an empty string when the
 * text has nothing to format (show it as plain text then). Links get
 * href="<the url>", hidden spoilers href="spoiler:".
 */
std::string toHtml(std::string_view text, const Options& options = {});

/// Whether `text` contains a link or a formatting marker that toHtml() would render.
bool hasFormatting(std::string_view text);

/// The text a reader sees: markers removed (for previews and notifications), links kept.
std::string toPlain(std::string_view text);

}  // namespace redvault::model::text_format
