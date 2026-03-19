#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  wrapText();
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
// Layout helpers — shared setup
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapText() {
  layoutLines.clear();

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;

  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Choose rendering path based on dictionary content type
  const DictInfo info = Dictionary::readInfo(Dictionary::getActivePath());
  if (info.valid && info.sametypesequence[0] == 'h') {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = (static_cast<int>(layoutLines.size()) + linesPerPage - 1) / linesPerPage;
  if (totalPages < 1) totalPages = 1;
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapHtml() {
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;

  // Indent step: 3 spaces worth of pixels at regular weight
  const int indentStep = renderer.getTextWidth(readerFontId, "   ");
  const int bulletWidth = renderer.getTextWidth(readerFontId, "- ");

  // Heap-allocate the renderer — textBuf[8192] is too large for the stack
  auto htmlRenderer = std::make_unique<DictHtmlRenderer>();
  const auto& spans = htmlRenderer->render(definition.c_str(), static_cast<int>(definition.size()));

  LayoutLine currentLine;
  int currentX = 0;

  auto flushLine = [&]() {
    if (!currentLine.segments.empty()) {
      layoutLines.push_back(std::move(currentLine));
      currentLine = LayoutLine{};
    }
  };

  auto startLine = [&](uint8_t indent, bool listItem) {
    currentLine.indentLevel = indent;
    currentLine.isListItem = listItem;
    currentX = indent * indentStep + (listItem ? bulletWidth : 0);
  };

  auto appendToLine = [&](const std::string& text, EpdFontFamily::Style style, int width) {
    if (!currentLine.segments.empty() && currentLine.segments.back().style == style) {
      currentLine.segments.back().text += text;
    } else {
      currentLine.segments.push_back({text, style});
    }
    currentX += width;
  };

  startLine(0, false);

  for (const auto& span : spans) {
    if (!span.text || span.text[0] == '\0') continue;

    EpdFontFamily::Style style;
    if (span.bold && span.italic) {
      style = EpdFontFamily::BOLD_ITALIC;
    } else if (span.bold) {
      style = EpdFontFamily::BOLD;
    } else if (span.italic) {
      style = EpdFontFamily::ITALIC;
    } else {
      style = EpdFontFamily::REGULAR;
    }

    if (span.newlineBefore) {
      flushLine();
      startLine(span.indentLevel, span.isListItem);
    }

    int spanWidth = renderer.getTextWidth(readerFontId, span.text, style);
    if (currentX + spanWidth <= maxWidth) {
      // Fast path: entire span fits on the current line.
      appendToLine(std::string(span.text), style, spanWidth);
    } else {
      // Word-wrap within the span.
      // Each word is placed on the current line if it fits; otherwise a new
      // continuation line is started. A word that is wider than maxWidth on
      // its own is still placed (can't break within a word).
      const char* p = span.text;
      while (*p) {
        bool hadSpace = false;
        while (*p == ' ') { hadSpace = true; ++p; }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        std::string tok(tokStart, p - tokStart);

        bool lineIsEmpty = currentLine.segments.empty();
        std::string candidate = (!lineIsEmpty && hadSpace) ? " " + tok : tok;
        int candidateWidth = renderer.getTextWidth(readerFontId, candidate.c_str(), style);

        if (currentX + candidateWidth > maxWidth && !lineIsEmpty) {
          flushLine();
          startLine(span.indentLevel, false);
          candidate = tok;
          candidateWidth = renderer.getTextWidth(readerFontId, tok.c_str(), style);
        }

        appendToLine(candidate, style, candidateWidth);
      }
    }
  }

  flushLine();
  // htmlRenderer freed here; span text has been copied into layoutLines
}

// ---------------------------------------------------------------------------
// Plain text path: word-wrap into single-segment REGULAR lines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapPlain() {
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;

  std::string currentWord;
  std::string currentLineText;

  auto flushLine = [&]() {
    LayoutLine line;
    line.segments.push_back({currentLineText, EpdFontFamily::REGULAR});
    layoutLines.push_back(std::move(line));
    currentLineText.clear();
  };

  for (size_t i = 0; i <= definition.size(); i++) {
    char c = (i < definition.size()) ? definition[i] : '\0';

    if (c == '\n' || c == '\0') {
      if (!currentWord.empty()) {
        if (currentLineText.empty()) {
          currentLineText = currentWord;
        } else {
          std::string test = currentLineText + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLineText = test;
          } else {
            flushLine();
            currentLineText = currentWord;
          }
        }
        currentWord.clear();
      }
      flushLine();
    } else if (c == ' ') {
      if (!currentWord.empty()) {
        if (currentLineText.empty()) {
          currentLineText = currentWord;
        } else {
          std::string test = currentLineText + " " + currentWord;
          if (renderer.getTextWidth(readerFontId, test.c_str()) <= maxWidth) {
            currentLineText = test;
          } else {
            flushLine();
            currentLineText = currentWord;
          }
        }
        currentWord.clear();
      }
    } else {
      currentWord += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Input loop
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::loop() {
  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevPage && currentPage > 0) {
    currentPage--;
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && showDoneButton) {
    setResult(ActivityResult{});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(readerFontId);
  const int indentStep = renderer.getTextWidth(readerFontId, "   ");

  // Header
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());
  const int bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Body: draw layout lines for the current page
  const int startLine = currentPage * linesPerPage;
  for (int i = 0; i < linesPerPage && (startLine + i) < static_cast<int>(layoutLines.size()); i++) {
    const LayoutLine& line = layoutLines[startLine + i];
    const int y = bodyStartY + i * lineHeight;
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      renderer.drawText(readerFontId, x, y, "- ");
      x += renderer.getTextWidth(readerFontId, "- ");
    }

    for (const auto& seg : line.segments) {
      renderer.drawText(readerFontId, x, y, seg.text.c_str(), true, seg.style);
      x += renderer.getTextWidth(readerFontId, seg.text.c_str(), seg.style);
    }
  }

  // Pagination indicator
  if (totalPages > 1) {
    std::string pageInfo = std::to_string(currentPage + 1) + "/" + std::to_string(totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo.c_str());
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing,
                      pageInfo.c_str());
  }

  // Button hints
  const char* btn2 = showDoneButton ? tr(STR_DONE) : "";
  const char* btn3 = totalPages > 1 ? tr(STR_PREV_NEXT) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_NEXT_PREV) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
