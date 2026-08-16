#include "MarkdownReaderActivity.h"

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "ProgressFile.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;
constexpr uint32_t CACHE_MAGIC = 0x4D444B49;  // "MDKI"
constexpr uint8_t CACHE_VERSION = 1;

// Pages per minute for the reader menu's auto-turn option; index 0 is "off".
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

// markdown::parseInline() emits EpdFontFamily style bits directly.
static_assert(markdown::STYLE_BOLD == EpdFontFamily::BOLD, "markdown bold bit must match EpdFontFamily");
static_assert(markdown::STYLE_ITALIC == EpdFontFamily::ITALIC, "markdown italic bit must match EpdFontFamily");
static_assert(markdown::STYLE_STRIKETHROUGH == EpdFontFamily::STRIKETHROUGH,
              "markdown strikethrough bit must match EpdFontFamily");

// Indent applied per list/quote nesting level, and the gap between a quote or
// code bar and its text. Both scale with the font so they track the text size.
int indentStep(const int lineHeight) { return std::max(8, lineHeight); }
int barGap(const int lineHeight) { return std::max(6, lineHeight * 2 / 3); }

bool isUtf8Continuation(const char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

EpdFontFamily::Style toStyle(const uint8_t bits) { return static_cast<EpdFontFamily::Style>(bits); }

// Leading spaces of a code line become indent pixels, so indentation survives
// without the wrapper having to preserve runs of spaces.
size_t codeLeadingSpaces(std::string_view content) {
  size_t n = 0;
  while (n < content.size() && content[n] == ' ') n++;
  return n;
}
}  // namespace

bool MarkdownReaderActivity::loadBook() {
  doc = makeUniqueNoThrow<Txt>(bookPath, "/.crosspoint");
  if (!doc) {
    LOG_ERR("MDR", "OOM: Txt object");
    return false;
  }
  if (!doc->load()) {
    LOG_ERR("MDR", "Failed to load markdown file");
    return false;
  }
  doc->setupCacheDir();
  return true;
}

void MarkdownReaderActivity::initializeReader(GfxRenderer& renderer) {
  if (initialized) return;

  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  lineHeight = renderer.getLineHeight(cachedFontId);
  if (lineHeight < 1) lineHeight = 1;

  LOG_DBG("MDR", "Viewport: %dx%d, line height: %d", viewportWidth, viewportHeight, lineHeight);

  chunkBuffer = makeUniqueNoThrow<uint8_t[]>(CHUNK_SIZE + 1);
  if (!chunkBuffer) {
    // Without the window nothing can be laid out; stay initialized so the
    // render path reports an empty document instead of retrying every frame.
    LOG_ERR("MDR", "OOM: %zu byte chunk buffer", CHUNK_SIZE + 1);
    initialized = true;
    return;
  }

  if (!loadPageIndexCache()) {
    buildPageIndex(renderer);
    savePageIndexCache();
  }

  if (hasPendingRestore) {
    // Re-paginated under new settings: page numbers no longer mean the same
    // thing, so land on whichever page now covers the offset we were reading.
    hasPendingRestore = false;
    currentPage = 0;
    for (size_t i = 0; i < pageAnchors.size(); i++) {
      if (pageAnchors[i].offset > pendingRestoreOffset) break;
      currentPage = static_cast<int>(i);
    }
    saveProgress();
  } else {
    loadProgress();
  }
  initialized = true;
}

void MarkdownReaderActivity::buildPageIndex(GfxRenderer& renderer) {
  pageAnchors.clear();
  pageAnchors.push_back(PageAnchor{});

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  const size_t fileSize = doc->getFileSize();
  PageAnchor anchor{};
  while (anchor.offset < fileSize) {
    PageAnchor next{};
    scratchLines.clear();
    if (!layoutPage(renderer, anchor, scratchLines, next)) break;

    // No forward progress means a page could not hold even one line; stop
    // rather than index the same offset forever.
    if (next.offset == anchor.offset && next.skipLines == anchor.skipLines) break;

    anchor = next;
    if (anchor.offset < fileSize) pageAnchors.push_back(anchor);

    if (pageAnchors.size() % 20 == 0) vTaskDelay(1);
  }

  scratchLines.clear();
  scratchLines.shrink_to_fit();
  totalPages = static_cast<int>(pageAnchors.size());
  LOG_DBG("MDR", "Built page index: %d pages", totalPages);
}

bool MarkdownReaderActivity::layoutPage(GfxRenderer& renderer, const PageAnchor& start,
                                        std::vector<DisplayLine>& outLines, PageAnchor& next) {
  const size_t fileSize = doc->getFileSize();
  if (start.offset >= fileSize || !chunkBuffer) return false;

  const size_t chunkSize = std::min(CHUNK_SIZE, fileSize - static_cast<size_t>(start.offset));
  if (!doc->readContent(chunkBuffer.get(), start.offset, chunkSize)) return false;
  chunkBuffer[chunkSize] = '\0';

  if (renderer.isSdCardFont(cachedFontId)) {
    // Emphasis can select any of the four variants, so prepare them all.
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(chunkBuffer.get()), /*styleMask=*/0x0F);
  }

  const std::string_view chunk(reinterpret_cast<const char*>(chunkBuffer.get()), chunkSize);
  markdown::State state{start.inCodeFence};

  size_t pos = 0;
  int usedHeight = 0;
  bool firstOnPage = true;
  bool prevWasBlank = false;
  uint16_t skipLines = start.skipLines;

  while (pos < chunkSize) {
    size_t lineEnd = chunk.find('\n', pos);
    bool atEof = false;
    if (lineEnd == std::string_view::npos) {
      if (start.offset + chunkSize >= fileSize) {
        lineEnd = chunkSize;  // last line of the file, no trailing newline
        atEof = true;
      } else if (pos > 0) {
        break;  // partial line: let the next page start at it
      } else {
        // A single source line longer than the chunk. Break it at a UTF-8
        // boundary and let the remainder be re-parsed as a fresh line: no
        // content is lost, only this pathological line's block styling.
        lineEnd = chunkSize;
        while (lineEnd > pos + 1 && isUtf8Continuation(chunk[lineEnd])) lineEnd--;
      }
    }

    std::string_view line = chunk.substr(pos, lineEnd - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    const markdown::State stateBeforeLine = state;
    const markdown::Block block = markdown::parseLine(line, state);

    if (block.type == markdown::BlockType::Blank) {
      prevWasBlank = true;
      pos = atEof ? chunkSize : lineEnd + 1;
      skipLines = 0;
      continue;
    }

    int blockHeight = 0;
    uint16_t emitted = 0;
    bool complete = false;
    layoutBlock(renderer, block, firstOnPage && outLines.empty(), skipLines, viewportHeight - usedHeight, outLines,
                blockHeight, emitted, complete);
    usedHeight += blockHeight;

    if (!complete) {
      // The block ran past the bottom of the page: the next one resumes at this
      // same source line, past the lines already shown.
      next.offset = static_cast<uint32_t>(start.offset + pos);
      next.skipLines = emitted;
      next.inCodeFence = stateBeforeLine.inCodeFence;
      return true;
    }

    prevWasBlank = false;
    firstOnPage = false;
    skipLines = 0;
    pos = atEof ? chunkSize : lineEnd + 1;

    if (usedHeight >= viewportHeight) break;
  }

  next.offset = static_cast<uint32_t>(start.offset + pos);
  next.skipLines = 0;
  next.inCodeFence = state.inCodeFence;
  return true;
}

void MarkdownReaderActivity::layoutBlock(GfxRenderer& renderer, const markdown::Block& block, const bool firstOnPage,
                                         const uint16_t skipLines, const int availableHeight,
                                         std::vector<DisplayLine>& outLines, int& usedHeight, uint16_t& emitted,
                                         bool& complete) const {
  usedHeight = 0;
  emitted = skipLines;
  complete = true;

  const int step = indentStep(lineHeight);
  const int gap = barGap(lineHeight);

  DisplayLine proto;
  proto.type = block.type;
  proto.headingLevel = block.headingLevel;

  uint8_t baseStyle = markdown::STYLE_REGULAR;
  std::string_view content = block.content;

  switch (block.type) {
    case markdown::BlockType::Heading:
      baseStyle = markdown::STYLE_BOLD;
      proto.ruleUnder = block.headingLevel <= 2;
      break;
    case markdown::BlockType::Quote:
      baseStyle = markdown::STYLE_ITALIC;
      proto.textIndent = static_cast<int16_t>(block.indentLevel * step + gap);
      break;
    case markdown::BlockType::Code: {
      const size_t lead = codeLeadingSpaces(content);
      proto.textIndent = static_cast<int16_t>(gap + lead * renderer.getSpaceWidth(cachedFontId));
      content.remove_prefix(lead);
      break;
    }
    case markdown::BlockType::BulletItem:
      proto.marker = "\xE2\x80\xA2";  // U+2022 BULLET
      proto.markerIndent = static_cast<int16_t>(block.indentLevel * step);
      break;
    case markdown::BlockType::OrderedItem:
      proto.marker = std::to_string(block.orderedNumber) + ".";
      proto.markerIndent = static_cast<int16_t>(block.indentLevel * step);
      break;
    default:
      break;
  }

  if (block.type == markdown::BlockType::BulletItem || block.type == markdown::BlockType::OrderedItem) {
    // Hanging indent: wrapped lines align under the text, not the marker.
    const int markerWidth = renderer.getTextAdvanceX(cachedFontId, proto.marker.c_str(), EpdFontFamily::REGULAR);
    proto.textIndent = static_cast<int16_t>(proto.markerIndent + markerWidth + renderer.getSpaceWidth(cachedFontId));
  }

  if (block.type == markdown::BlockType::Rule) {
    if (!firstOnPage) proto.topPad = static_cast<int16_t>(lineHeight / 3);
    const int height = proto.topPad + lineHeight;
    if (height > availableHeight && !outLines.empty()) {
      complete = false;
      return;
    }
    outLines.push_back(proto);
    usedHeight = height;
    emitted = 1;
    return;
  }

  std::vector<markdown::Run> runs;
  if (block.type == markdown::BlockType::Code) {
    // Code is verbatim: no emphasis, no link collapsing.
    markdown::Run run;
    run.text.assign(content);
    run.code = true;
    runs.push_back(std::move(run));
  } else {
    markdown::parseInline(content, runs);
  }

  const int maxWidth = std::max(1, viewportWidth - proto.textIndent);
  const int spaceWidth = renderer.getSpaceWidth(cachedFontId);

  // Wrap the runs into lines, carrying each word's style with it.
  std::vector<DisplayLine> wrapped;
  DisplayLine current = proto;
  int currentWidth = 0;

  auto flush = [&] {
    wrapped.push_back(std::move(current));
    current = proto;
    current.marker.clear();  // marker belongs to the first line only
    current.topPad = 0;
    currentWidth = 0;
  };

  auto appendWord = [&](const std::string& word, const uint8_t style, const bool code, const bool needSpace) {
    if (!current.runs.empty() && current.runs.back().style == style && current.runs.back().code == code) {
      if (needSpace) current.runs.back().text += ' ';
      current.runs.back().text += word;
    } else {
      markdown::Run run;
      run.style = style;
      run.code = code;
      if (needSpace) run.text += ' ';
      run.text += word;
      current.runs.push_back(std::move(run));
    }
  };

  std::string word;
  std::string head;
  for (const markdown::Run& run : runs) {
    size_t i = 0;
    while (i < run.text.size()) {
      while (i < run.text.size() && run.text[i] == ' ') i++;
      if (i >= run.text.size()) break;
      const size_t wordStart = i;
      while (i < run.text.size() && run.text[i] != ' ') i++;
      word.assign(run.text, wordStart, i - wordStart);

      int wordWidth = renderer.getTextAdvanceX(cachedFontId, word.c_str(), toStyle(run.style));
      const bool needSpace = !current.runs.empty();
      const int advance = (needSpace ? spaceWidth : 0) + wordWidth;

      if (currentWidth + advance > maxWidth && !current.runs.empty()) {
        flush();
      }

      // A word wider than the line (a URL, a CJK run) is split across lines at
      // the last UTF-8 boundary that still fits.
      while (wordWidth > maxWidth) {
        if (!current.runs.empty()) flush();
        size_t cut = 0;
        size_t probe = 0;
        while (probe < word.size()) {
          size_t next = probe + 1;
          while (next < word.size() && isUtf8Continuation(word[next])) next++;
          head.assign(word, 0, next);
          if (renderer.getTextAdvanceX(cachedFontId, head.c_str(), toStyle(run.style)) > maxWidth) break;
          cut = next;
          probe = next;
        }
        if (cut == 0) break;  // not even one character fits; let the line overflow
        head.assign(word, 0, cut);
        appendWord(head, run.style, run.code, false);
        word.erase(0, cut);
        flush();
        wordWidth = renderer.getTextAdvanceX(cachedFontId, word.c_str(), toStyle(run.style));
      }

      const bool space = !current.runs.empty();
      appendWord(word, run.style, run.code, space);
      currentWidth += (space ? spaceWidth : 0) + wordWidth;
    }
  }
  if (!current.runs.empty()) flush();

  if (wrapped.empty()) {
    emitted = skipLines;
    return;
  }

  // Spacing above the block, dropped when it opens the page so text starts
  // flush with the top margin.
  if (!firstOnPage && skipLines == 0) {
    switch (block.type) {
      case markdown::BlockType::Heading:
        wrapped.front().topPad = static_cast<int16_t>(block.headingLevel <= 2 ? lineHeight / 2 : lineHeight / 3);
        break;
      default:
        wrapped.front().topPad = static_cast<int16_t>(lineHeight / 3);
        break;
    }
  }

  for (size_t n = skipLines; n < wrapped.size(); n++) {
    DisplayLine& line = wrapped[n];
    if (n != 0) line.topPad = 0;
    const int height = lineTotalHeight(line);
    if (usedHeight + height > availableHeight && !outLines.empty()) {
      complete = false;
      return;
    }
    usedHeight += height;
    emitted = static_cast<uint16_t>(n + 1);
    outLines.push_back(std::move(line));
  }
}

int MarkdownReaderActivity::lineTotalHeight(const DisplayLine& line) const {
  // The divider under an H1/H2 lives in the gap that follows the heading.
  return line.topPad + lineHeight + (line.ruleUnder ? lineHeight / 4 : 0);
}

void MarkdownReaderActivity::drawLines(GfxRenderer& renderer, const bool withDecorations) const {
  const int left = cachedOrientedMarginLeft;
  const int gap = barGap(lineHeight);
  int y = cachedOrientedMarginTop;

  for (const DisplayLine& line : currentPageLines) {
    y += line.topPad;

    if (line.type == markdown::BlockType::Rule) {
      if (withDecorations) {
        const int ruleY = y + lineHeight / 2;
        renderer.drawLine(left, ruleY, left + viewportWidth, ruleY, true);
      }
      y += lineHeight;
      continue;
    }

    if (withDecorations && (line.type == markdown::BlockType::Quote || line.type == markdown::BlockType::Code)) {
      const int barX = left + line.textIndent - gap;
      renderer.drawLine(barX, y, barX, y + lineHeight, 2, true);
    }

    if (!line.marker.empty()) {
      renderer.drawText(cachedFontId, left + line.markerIndent, y, line.marker.c_str());
    }

    int x = left + line.textIndent;
    for (const markdown::Run& run : line.runs) {
      const auto style = toStyle(run.style);
      const int width = renderer.getTextAdvanceX(cachedFontId, run.text.c_str(), style);
      if (withDecorations && run.code) {
        renderer.fillRectDither(x, y, width, lineHeight, Color::LightGray);
      }
      renderer.drawText(cachedFontId, x, y, run.text.c_str(), true, style);
      x += width;
    }

    y += lineHeight;
    if (line.ruleUnder) {
      if (withDecorations) {
        const int ruleY = y + lineHeight / 8;
        renderer.drawLine(left, ruleY, left + viewportWidth, ruleY, true);
      }
      y += lineHeight / 4;
    }
  }
}

void MarkdownReaderActivity::renderBook() {
  if (!doc) return;

  if (!initialized) initializeReader(renderer);

  if (pageAnchors.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  currentPageLines.clear();
  PageAnchor next{};
  layoutPage(renderer, pageAnchors[currentPage], currentPageLines, next);

  renderer.clearScreen();
  renderPage(renderer);

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  saveProgress();
}

void MarkdownReaderActivity::renderPage(GfxRenderer& renderer) {
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawLines(renderer, /*withDecorations=*/false);  // scan pass
  scope.endScanAndPrewarm();

  drawLines(renderer, /*withDecorations=*/true);
  renderStatusBar();

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
    ReaderUtils::renderAntiAliased(renderer, [this, &renderer]() { drawLines(renderer, /*withDecorations=*/false); });
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}

void MarkdownReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = doc->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

bool MarkdownReaderActivity::pageTurn(const bool isForward) {
  if (!initialized) return false;
  if (isForward) {
    if (currentPage < totalPages) {
      currentPage++;
      return true;
    }
  } else if (currentPage > 0) {
    currentPage--;
    return true;
  }
  return false;
}

bool MarkdownReaderActivity::skipPages(const int amount) {
  if (!initialized) return false;
  int newPage = currentPage + amount;
  if (newPage < 0) newPage = 0;
  // Clamp to totalPages, not totalPages - 1: that value is the end-of-book
  // sentinel pageTurn() can reach, so a forward skip must reach it too.
  if (newPage > totalPages) newPage = totalPages;
  if (newPage == currentPage) return false;
  currentPage = newPage;
  return true;
}

bool MarkdownReaderActivity::isAtEndOfBook() const { return initialized && currentPage >= totalPages; }

void MarkdownReaderActivity::onReturnFromEndOfBook() { currentPage = totalPages > 0 ? totalPages - 1 : 0; }

void MarkdownReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(doc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("MDR", "Failed to save progress: page %d", currentPage);
  }
}

void MarkdownReaderActivity::loadProgress() {
  HalFile f;
  if (!Storage.openFileForRead("MDR", doc->getCachePath() + "/progress.bin", f)) return;

  uint8_t data[4];
  if (f.read(data, 4) != 4) return;
  currentPage = data[0] + (data[1] << 8);
  if (currentPage >= totalPages) currentPage = totalPages - 1;
  if (currentPage < 0) currentPage = 0;
  LOG_DBG("MDR", "Loaded progress: page %d/%d", currentPage, totalPages);
}

bool MarkdownReaderActivity::loadPageIndexCache() {
  HalFile f;
  if (!Storage.openFileForRead("MDR", doc->getCachePath() + "/index.bin", f)) {
    LOG_DBG("MDR", "No page index cache found");
    return false;
  }

  uint32_t magic = 0;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) return false;

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("MDR", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize = 0;
  serialization::readPod(f, fileSize);
  if (fileSize != doc->getFileSize()) return false;

  int32_t width = 0;
  serialization::readPod(f, width);
  if (width != viewportWidth) return false;

  int32_t height = 0;
  serialization::readPod(f, height);
  if (height != viewportHeight) return false;

  int32_t fontId = 0;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) return false;

  int32_t margin = 0;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) return false;

  uint32_t numPages = 0;
  serialization::readPod(f, numPages);

  pageAnchors.clear();
  pageAnchors.reserve(numPages);
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset = 0;
    uint16_t skipLines = 0;
    uint8_t inCodeFence = 0;
    serialization::readPod(f, offset);
    serialization::readPod(f, skipLines);
    serialization::readPod(f, inCodeFence);
    pageAnchors.push_back(PageAnchor{offset, skipLines, inCodeFence != 0});
  }

  totalPages = static_cast<int>(pageAnchors.size());
  LOG_DBG("MDR", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void MarkdownReaderActivity::savePageIndexCache() const {
  HalFile f;
  if (!Storage.openFileForWrite("MDR", doc->getCachePath() + "/index.bin", f)) {
    LOG_ERR("MDR", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(doc->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(viewportHeight));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, static_cast<uint32_t>(pageAnchors.size()));

  for (const PageAnchor& anchor : pageAnchors) {
    serialization::writePod(f, anchor.offset);
    serialization::writePod(f, anchor.skipLines);
    serialization::writePod(f, static_cast<uint8_t>(anchor.inCodeFence ? 1 : 0));
  }

  LOG_DBG("MDR", "Saved page index cache: %d pages", totalPages);
}

uint32_t MarkdownReaderActivity::currentOffset() const {
  if (pageAnchors.empty()) return 0;
  const int page = std::clamp(currentPage, 0, static_cast<int>(pageAnchors.size()) - 1);
  return pageAnchors[page].offset;
}

int MarkdownReaderActivity::bookProgressPercent() const {
  if (totalPages <= 0) return 0;
  const int percent = static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f);
  return std::clamp(percent, 0, 100);
}

void MarkdownReaderActivity::invalidateLayout() {
  {
    RenderLock lock(*this);
    pendingRestoreOffset = currentOffset();
    hasPendingRestore = true;
    // Dropping the index forces initializeReader() to re-measure the viewport
    // and re-paginate; the on-disk cache is rejected by its own font/margin
    // checks, so nothing stale survives.
    initialized = false;
    pageAnchors.clear();
    currentPageLines.clear();
    totalPages = 1;
  }
  requestUpdate();
}

void MarkdownReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) return;
  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, orientation);
  invalidateLayout();
}

void MarkdownReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }
  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;
}

void MarkdownReaderActivity::openReaderMenu() {
  // No chapters, bookmarks, word lookup or KOReader sync for markdown: those
  // rows are hidden rather than shown inert.
  constexpr EpubReaderMenuActivity::Features features{/*chapters=*/false, /*bookmarks=*/false, /*dictionary=*/false,
                                                      /*sync=*/false};
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, doc ? doc->getTitle() : "", currentPage + 1,
                                               totalPages, bookProgressPercent(), SETTINGS.orientation,
                                               /*hasFootnotes=*/false, /*hasBookmarks=*/false, features),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void MarkdownReaderActivity::onReaderMenuConfirm(const EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS:
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               // TextSettingsActivity saves each change itself; font, size,
                               // spacing and margin all change pagination.
                               invalidateLayout();
                             });
      break;

    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT:
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, bookProgressPercent()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const int percent = std::clamp(std::get<PercentResult>(result.data).percent, 0, 100);
            const int page = totalPages > 0 ? (percent * totalPages) / 100 : 0;
            currentPage = std::clamp(page, 0, totalPages > 0 ? totalPages - 1 : 0);
            requestUpdate();
          });
      break;

    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      // The QR carries the page's plain text, emphasis markers already resolved.
      // currentPageLines belongs to the render task; copy it out under the lock.
      std::string pageText;
      {
        RenderLock lock(*this);
        for (const DisplayLine& line : currentPageLines) {
          for (const markdown::Run& run : line.runs) pageText += run.text;
          pageText += '\n';
        }
      }
      if (!pageText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, pageText),
                               [](const ActivityResult&) {});
        break;
      }
      requestUpdate();
      break;
    }

    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;

    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (doc) {
          doc->clearCache();
          doc->setupCacheDir();
        }
      }
      // clearCache() takes progress.bin along with the index; the settings are
      // unchanged, so the rebuilt index keeps the same page numbering.
      saveProgress();
      onGoHome();
      return;
    }

    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      return;

    // Applied by the menu before it closed (night mode, frontlight), handled
    // via the MenuResult fields (orientation, auto page turn), or hidden for
    // this format entirely.
    default:
      break;
  }
}

bool MarkdownReaderActivity::handleFormatInput() {
  // At the end of the book Confirm belongs to the end-of-book options screen.
  if (!doc || isAtEndOfBook()) return false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openReaderMenu();
    return true;
  }
  return false;
}

void MarkdownReaderActivity::loop() {
  if (automaticPageTurnActive && initialized && millis() - lastPageTurnTime >= pageTurnDuration) {
    lastPageTurnTime = millis();
    if (pageTurn(true)) {
      requestUpdate();
    } else {
      automaticPageTurnActive = false;
    }
  }
  ReaderActivity::loop();
}

ScreenshotInfo MarkdownReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Markdown;
  if (doc) {
    const std::string t = doc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
