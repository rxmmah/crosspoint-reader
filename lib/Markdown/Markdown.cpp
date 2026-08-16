#include "Markdown.h"

#include <algorithm>
#include <cctype>

namespace markdown {
namespace {

constexpr size_t CODE_INDENT_SPACES = 4;
constexpr uint8_t MAX_HEADING_LEVEL = 6;
// One nesting level per this many leading spaces, matching how most editors
// indent nested lists. Tabs count as one full level.
constexpr size_t LIST_INDENT_SPACES = 2;

bool isSpace(const char c) { return c == ' ' || c == '\t'; }

// Leading whitespace measured in spaces, tabs counting as CODE_INDENT_SPACES.
size_t leadingIndent(std::string_view line, size_t& firstNonSpace) {
  size_t indent = 0;
  size_t i = 0;
  for (; i < line.size() && isSpace(line[i]); i++) {
    indent += line[i] == '\t' ? CODE_INDENT_SPACES : 1;
  }
  firstNonSpace = i;
  return indent;
}

std::string_view trimTrailing(std::string_view s) {
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  return trimTrailing(s);
}

// A fence is three or more backticks or tildes; anything after them on an
// opening fence is an info string we drop.
bool isFenceDelimiter(std::string_view rest) {
  if (rest.size() < 3) return false;
  const char c = rest.front();
  if (c != '`' && c != '~') return false;
  size_t run = 0;
  while (run < rest.size() && rest[run] == c) run++;
  if (run < 3) return false;
  // A closing fence must carry nothing else; an opening one may carry a
  // language tag, which parseLine() discards either way.
  return c == '`' ? rest.find('`', run) == std::string_view::npos : true;
}

// --- / *** / ___ , three or more, spaces allowed between.
bool isThematicBreak(std::string_view rest) {
  if (rest.empty()) return false;
  const char c = rest.front();
  if (c != '-' && c != '*' && c != '_') return false;
  size_t count = 0;
  for (const char ch : rest) {
    if (ch == c) {
      count++;
    } else if (!isSpace(ch)) {
      return false;
    }
  }
  return count >= 3;
}

// "1. " or "1) " -> the number, with the marker length written to markerLen.
bool parseOrderedMarker(std::string_view rest, uint32_t& number, size_t& markerLen) {
  size_t i = 0;
  uint32_t value = 0;
  while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
    // Cap rather than overflow: a 10-digit "number" is not a list marker.
    if (i >= 9) return false;
    value = value * 10 + static_cast<uint32_t>(rest[i] - '0');
    i++;
  }
  if (i == 0 || i >= rest.size()) return false;
  if (rest[i] != '.' && rest[i] != ')') return false;
  i++;
  if (i < rest.size() && !isSpace(rest[i])) return false;
  while (i < rest.size() && isSpace(rest[i])) i++;
  number = value;
  markerLen = i;
  return true;
}

}  // namespace

Block parseLine(const std::string_view line, State& state) {
  Block block;

  size_t firstNonSpace = 0;
  const size_t indent = leadingIndent(line, firstNonSpace);
  const std::string_view rest = line.substr(firstNonSpace);

  // A fence toggles the code block and never renders itself.
  if (isFenceDelimiter(rest)) {
    state.inCodeFence = !state.inCodeFence;
    return block;  // Blank
  }

  if (state.inCodeFence) {
    block.type = BlockType::Code;
    // Verbatim, including indentation: code layout is not reflowed.
    block.content = trimTrailing(line);
    return block;
  }

  if (rest.empty()) {
    return block;  // Blank
  }

  // Indented code only counts outside a list, where 4 spaces mean continuation.
  if (indent >= CODE_INDENT_SPACES) {
    block.type = BlockType::Code;
    block.content = trimTrailing(line.substr(std::min(firstNonSpace, CODE_INDENT_SPACES)));
    return block;
  }

  if (isThematicBreak(rest)) {
    block.type = BlockType::Rule;
    return block;
  }

  if (rest.front() == '#') {
    size_t level = 0;
    while (level < rest.size() && rest[level] == '#') level++;
    if (level <= MAX_HEADING_LEVEL && level < rest.size() && isSpace(rest[level])) {
      block.type = BlockType::Heading;
      block.headingLevel = static_cast<uint8_t>(level);
      std::string_view content = trim(rest.substr(level));
      // Optional closing sequence: "## Title ##".
      while (!content.empty() && content.back() == '#') content.remove_suffix(1);
      block.content = trimTrailing(content);
      return block;
    }
  }

  if (rest.front() == '>') {
    block.type = BlockType::Quote;
    block.indentLevel = static_cast<uint8_t>(indent / LIST_INDENT_SPACES);
    std::string_view content = rest.substr(1);
    if (!content.empty() && content.front() == ' ') content.remove_prefix(1);
    block.content = trimTrailing(content);
    return block;
  }

  if ((rest.front() == '-' || rest.front() == '*' || rest.front() == '+') && rest.size() > 1 && isSpace(rest[1])) {
    block.type = BlockType::BulletItem;
    block.indentLevel = static_cast<uint8_t>(indent / LIST_INDENT_SPACES);
    block.content = trim(rest.substr(1));
    return block;
  }

  uint32_t number = 0;
  size_t markerLen = 0;
  if (parseOrderedMarker(rest, number, markerLen)) {
    block.type = BlockType::OrderedItem;
    block.indentLevel = static_cast<uint8_t>(indent / LIST_INDENT_SPACES);
    block.orderedNumber = number;
    block.content = trimTrailing(rest.substr(markerLen));
    return block;
  }

  block.type = BlockType::Paragraph;
  block.content = trimTrailing(rest);
  return block;
}

namespace {

// A delimiter opens emphasis only when it butts against following text, and
// closes only when it butts against preceding text. That single rule keeps
// "2 * 3 * 4" and snake_case words out of the emphasis path without needing
// CommonMark's full flanking analysis.
bool canOpen(std::string_view text, const size_t pos, const size_t runLen) {
  const size_t after = pos + runLen;
  return after < text.size() && !isSpace(text[after]);
}

bool canClose(std::string_view text, const size_t pos) { return pos > 0 && !isSpace(text[pos - 1]); }

// '_' inside a word is a literal underscore (snake_case), unlike '*'.
bool underscoreAtWordBoundary(std::string_view text, const size_t pos, const size_t runLen) {
  const bool leftFree = pos == 0 || isSpace(text[pos - 1]) || !isalnum(static_cast<unsigned char>(text[pos - 1]));
  const size_t after = pos + runLen;
  const bool rightFree =
      after >= text.size() || isSpace(text[after]) || !isalnum(static_cast<unsigned char>(text[after]));
  return leftFree || rightFree;
}

// True when a matching closing delimiter exists later in the line. Without this
// an unpaired "**" would style everything after it, so a stray delimiter is
// left as literal text instead.
bool hasCloser(std::string_view text, const size_t from, const char delim, const size_t delimLen) {
  size_t i = from;
  while (i < text.size()) {
    if (text[i] == '\\') {
      i += 2;
      continue;
    }
    if (text[i] != delim) {
      i++;
      continue;
    }
    size_t run = 0;
    while (i + run < text.size() && text[i + run] == delim) run++;
    if (run >= delimLen && canClose(text, i) &&
        (delim != '_' || underscoreAtWordBoundary(text, i, std::min(run, delimLen)))) {
      return true;
    }
    i += run;
  }
  return false;
}

void pushRun(std::vector<Run>& runs, std::string& pending, const uint8_t style, const bool code) {
  if (pending.empty()) return;
  if (!runs.empty() && runs.back().style == style && runs.back().code == code) {
    runs.back().text += pending;
  } else {
    Run run;
    run.text = pending;
    run.style = style;
    run.code = code;
    runs.push_back(std::move(run));
  }
  pending.clear();
}

}  // namespace

void parseInline(const std::string_view text, std::vector<Run>& runs) {
  runs.clear();

  std::string pending;
  pending.reserve(text.size());
  uint8_t style = STYLE_REGULAR;

  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];

    if (c == '\\' && i + 1 < text.size()) {
      // Escapes only apply to punctuation; "\n" in prose stays literal.
      const char next = text[i + 1];
      if (!isalnum(static_cast<unsigned char>(next)) && !isSpace(next)) {
        pending += next;
        i += 2;
        continue;
      }
    }

    // Code spans win over every other marker and take their content verbatim.
    if (c == '`') {
      size_t open = 0;
      while (i + open < text.size() && text[i + open] == '`') open++;
      const std::string_view fence = text.substr(i, open);
      const size_t close = text.find(fence, i + open);
      if (close != std::string_view::npos) {
        pushRun(runs, pending, style, false);
        std::string_view code = text.substr(i + open, close - i - open);
        // A single leading/trailing space is padding that lets a span hold a
        // backtick ("`` ` ``"); anything more is content.
        if (code.size() > 1 && code.front() == ' ' && code.back() == ' ') {
          code.remove_prefix(1);
          code.remove_suffix(1);
        }
        pending.assign(code);
        pushRun(runs, pending, style, true);
        i = close + open;
        continue;
      }
    }

    if (c == '~' && i + 1 < text.size() && text[i + 1] == '~') {
      const bool active = (style & STYLE_STRIKETHROUGH) != 0;
      if (active ? canClose(text, i) : (canOpen(text, i, 2) && hasCloser(text, i + 2, '~', 2))) {
        pushRun(runs, pending, style, false);
        style ^= STYLE_STRIKETHROUGH;
        i += 2;
        continue;
      }
    }

    if (c == '*' || c == '_') {
      size_t run = 0;
      while (i + run < text.size() && text[i + run] == c) run++;
      const size_t delimLen = run >= 2 ? 2 : 1;
      const uint8_t bit = delimLen == 2 ? STYLE_BOLD : STYLE_ITALIC;
      const bool active = (style & bit) != 0;
      const bool boundaryOk = c == '*' || underscoreAtWordBoundary(text, i, delimLen);
      const bool opens = canOpen(text, i, delimLen) && hasCloser(text, i + delimLen, c, delimLen);
      if (boundaryOk && (active ? canClose(text, i) : opens)) {
        pushRun(runs, pending, style, false);
        style ^= bit;
        i += delimLen;
        continue;
      }
    }

    // Links and images collapse to their label; the URL is unusable on-device.
    if (c == '!' && i + 1 < text.size() && text[i + 1] == '[') {
      const size_t labelEnd = text.find(']', i + 2);
      if (labelEnd != std::string_view::npos && labelEnd + 1 < text.size() && text[labelEnd + 1] == '(') {
        const size_t urlEnd = text.find(')', labelEnd + 2);
        if (urlEnd != std::string_view::npos) {
          pending.append(text.substr(i + 2, labelEnd - i - 2));
          i = urlEnd + 1;
          continue;
        }
      }
    }

    if (c == '[') {
      const size_t labelEnd = text.find(']', i + 1);
      if (labelEnd != std::string_view::npos && labelEnd + 1 < text.size() && text[labelEnd + 1] == '(') {
        const size_t urlEnd = text.find(')', labelEnd + 2);
        if (urlEnd != std::string_view::npos) {
          // Recurse over the label so "[**bold** link](url)" keeps its emphasis.
          std::vector<Run> labelRuns;
          parseInline(text.substr(i + 1, labelEnd - i - 1), labelRuns);
          pushRun(runs, pending, style, false);
          for (Run& run : labelRuns) {
            pending = std::move(run.text);
            pushRun(runs, pending, static_cast<uint8_t>(style | run.style), run.code);
          }
          i = urlEnd + 1;
          continue;
        }
      }
    }

    pending += c;
    i++;
  }

  pushRun(runs, pending, style, false);
}

}  // namespace markdown
