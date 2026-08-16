#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Streaming CommonMark subset for the markdown reader.
//
// The reader paginates by byte offset into the source file, so this parser is
// deliberately line-at-a-time: classifying a line needs nothing but that line
// plus a one-byte carry (`State`), which lets the reader resume layout at any
// indexed page without re-reading the file from the start.
namespace markdown {

enum class BlockType : uint8_t {
  Blank,
  Paragraph,
  Heading,
  BulletItem,
  OrderedItem,
  Quote,
  Code,  // inside a ``` fence, or an indented (4-space) code line
  Rule,
};

// Inline style bits. The values match EpdFontFamily::Style so a run's style can
// be handed straight to GfxRenderer::drawText; MarkdownReaderActivity holds the
// static_asserts that pin them together.
inline constexpr uint8_t STYLE_REGULAR = 0;
inline constexpr uint8_t STYLE_BOLD = 1;
inline constexpr uint8_t STYLE_ITALIC = 2;
inline constexpr uint8_t STYLE_STRIKETHROUGH = 8;

// Carried across lines, and persisted per page in the reader's index so layout
// can restart mid-document. Keep it byte-packed and POD.
struct State {
  bool inCodeFence = false;
};

struct Block {
  BlockType type = BlockType::Blank;
  // Points into the line handed to parseLine() — the marker ("## ", "- ", "> ")
  // is already stripped. Never outlives the caller's buffer.
  std::string_view content;
  uint8_t headingLevel = 0;  // 1..6, Heading only
  uint8_t indentLevel = 0;   // list/quote nesting depth, 0-based
  uint32_t orderedNumber = 0;
};

// Classify one source line (newline already stripped) and advance `state` across
// fence open/close. A fence delimiter line itself yields BlockType::Blank with
// empty content, so it occupies no space on the page.
Block parseLine(std::string_view line, State& state);

struct Run {
  std::string text;
  uint8_t style = STYLE_REGULAR;
  bool code = false;  // `span`: rendered verbatim on a shaded background
};

// Split `text` into styled runs, resolving emphasis, code spans, strikethrough,
// links (kept as their label) and backslash escapes. `runs` is cleared first.
// Adjacent runs sharing a style are merged so the renderer draws fewer pieces.
void parseInline(std::string_view text, std::vector<Run>& runs);

}  // namespace markdown
