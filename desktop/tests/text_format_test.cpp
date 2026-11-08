// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Message formatting (model/text_format.hpp): markers, word boundaries,
// links, spoilers, and above all that nothing a sender types becomes markup.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "redvault/model/text_format.hpp"

using namespace redvault::model;

namespace {

std::string html(std::string_view text) {
    return text_format::toHtml(text);
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("plain text has nothing to format", "[format][text]") {
    CHECK(html("hello there") == "");
    CHECK(html("snake_case_name and 2*3*4 and file_name.txt") == "");
    CHECK(html("a * b * c") == "");
    CHECK(html("**double**") == "");
    CHECK(html("*open but never closed") == "");
    CHECK(html("* spaced *") == "");
    CHECK_FALSE(text_format::hasFormatting("just words"));
}

TEST_CASE("markers become bold, italic, strike, monospace and spoilers", "[format][text]") {
    CHECK(contains(html("a *bold* word"), "a <b>bold</b> word"));
    CHECK(contains(html("_italic_"), "<i>italic</i>"));
    CHECK(contains(html("~gone~"), "<s>gone</s>"));
    CHECK(contains(html("run `make -j8` now"), "<span style=\"font-family:'IBM Plex Mono'\">make -j8</span>"));
    CHECK(contains(html("(*inside brackets*)"), "(<b>inside brackets</b>)"));
    CHECK(contains(html("*bold and _italic_*"), "<b>bold and <i>italic</i></b>"));
    // Nothing is formatted inside monospace.
    CHECK(contains(html("`*not bold*`"), ">*not bold*</span>"));
    // Markers don't span lines.
    CHECK(html("*one\ntwo*") == "");
    const auto spoiler = html("the killer is ||the butler||!");
    CHECK(contains(spoiler, "href=\"spoiler:\""));
    CHECK(contains(spoiler, ">the butler</span></a>!"));
    text_format::Options reveal;
    reveal.revealSpoilers = true;
    const auto shown = text_format::toHtml("||secret *bold*||", reveal);
    CHECK_FALSE(contains(shown, "spoiler:"));
    CHECK(contains(shown, "<b>bold</b>"));
}

TEST_CASE("everything the sender typed is escaped", "[format][text]") {
    const auto out = html("*<img src=x onerror=alert(1)>* & \"q\" 'a' <b>raw</b>");
    CHECK(contains(out, "<b>&lt;img src=x onerror=alert(1)&gt;</b>"));
    CHECK(contains(out, "&amp; &quot;q&quot; &#39;a&#39; &lt;b&gt;raw&lt;/b&gt;"));
    CHECK_FALSE(contains(out, "<img"));
    // Newlines become line breaks inside formatted text.
    CHECK(contains(html("*a*\nb"), "<b>a</b><br>b"));
}

TEST_CASE("http and https links are clickable, nothing else", "[format][text]") {
    CHECK(contains(html("see https://example.com/a?b=1&c=2."),
                   "<a href=\"https://example.com/a?b=1&amp;c=2\" style=\"color:#86D9AE\">https://example.com/a?b=1&amp;c=2</a>."));
    CHECK(contains(html("(http://example.com/x_(y))"), ">http://example.com/x_(y)</a>)"));
    CHECK(contains(html("*https://example.com*"), "<b><a href=\"https://example.com\""));
    CHECK(html("javascript:alert(1) file:///c:/x ftp://x") == "");
    CHECK(html("xhttps://example.com") == "");
    CHECK(contains(html("https://example.com\"onmouseover=x"), "href=\"https://example.com\""));
}

TEST_CASE("mentions of group members are highlighted, yours more", "[format][text]") {
    text_format::Options options;
    options.mentions = {"bob", "bobby", "Ann Lee", "me"};
    options.selfHandle = "me";
    const auto out = text_format::toHtml("hi @Bob and @bobby, ask @Ann Lee; email@bob.com @me @carol", options);
    CHECK(contains(out, "hi <span style=\"color:#86D9AE;font-weight:600\">@Bob</span>"));
    CHECK(contains(out, ">@bobby</span>"));
    CHECK(contains(out, ">@Ann Lee</span>;"));
    CHECK(contains(out, "email@bob.com"));  // not a mention: it's in the middle of a word
    CHECK(contains(out, "background-color:#244536\">@me</span>"));
    CHECK(contains(out, " @carol"));        // not a member
    CHECK_FALSE(contains(out, ">@carol"));
    CHECK(text_format::mentions("ping @me please", "me"));
    CHECK_FALSE(text_format::mentions("ping @meg", "me"));
    // Without a member list nothing is treated as a mention.
    CHECK(html("hi @bob") == "");
}

TEST_CASE("plain previews drop markers and hide spoilers", "[format][text]") {
    CHECK(text_format::toPlain("*bold* and _it_ ~s~ `c` ||secret||") == "bold and it s c \xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    CHECK(text_format::toPlain("snake_case") == "snake_case");
}

TEST_CASE("hostile input stays fast and shallow", "[format][text]") {
    std::string many;
    for (int i = 0; i < 20000; ++i) many += " *a";
    std::string deep;
    for (int i = 0; i < 5000; ++i) deep += "*_";
    deep += "x";
    for (int i = 0; i < 5000; ++i) deep += "_*";
    const auto start = std::chrono::steady_clock::now();
    (void)html(many);
    (void)html(deep);
    (void)text_format::toPlain(deep);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
