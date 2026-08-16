#pragma once

#include <Markdown.h>
#include <Txt.h>

#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"

// Renders .md files with markdown styling: headings, emphasis, lists, quotes,
// code blocks and rules.
//
// Pagination follows the same streaming model as TxtReaderActivity — an index
// of byte offsets, laid out on demand — but a page break can land inside a long
// paragraph, so an anchor also carries how many of that source line's wrapped
// lines earlier pages already showed. See PageAnchor.
class MarkdownReaderActivity final : public ReaderActivity {
  // Where a page starts. `skipLines` re-lays out the source line at `offset`
  // and drops the leading display lines a previous page consumed, which keeps
  // every anchor pointing at a real source-line boundary.
  struct PageAnchor {
    uint32_t offset = 0;
    uint16_t skipLines = 0;
    bool inCodeFence = false;
  };

  // One drawn line: the wrapped text runs plus the block decoration around them.
  struct DisplayLine {
    std::vector<markdown::Run> runs;
    markdown::BlockType type = markdown::BlockType::Paragraph;
    int16_t textIndent = 0;  // from the left content edge
    int16_t topPad = 0;      // blank space reserved above this line
    uint8_t headingLevel = 0;
    bool ruleUnder = false;  // divider below an H1/H2
    // List marker ("•", "3.") drawn once, in the hanging indent.
    std::string marker;
    int16_t markerIndent = 0;
  };

  std::unique_ptr<Txt> doc;

  int currentPage = 0;
  int totalPages = 1;

  std::vector<PageAnchor> pageAnchors;
  std::vector<DisplayLine> currentPageLines;
  // Reused by buildPageIndex() so indexing a long document does not churn the
  // heap once per page.
  std::vector<DisplayLine> scratchLines;
  // Source-text window, allocated once in initializeReader() and reused by every
  // layoutPage() call rather than reallocated per page.
  std::unique_ptr<uint8_t[]> chunkBuffer;

  int viewportWidth = 0;
  int viewportHeight = 0;
  int lineHeight = 0;
  bool initialized = false;

  // Cached settings for page-index cache validation
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void initializeReader(GfxRenderer& renderer);
  void buildPageIndex(GfxRenderer& renderer);

  // Lays out one page starting at `start`, appending to `outLines` and writing
  // where the next page begins. Returns false at end of file.
  bool layoutPage(GfxRenderer& renderer, const PageAnchor& start, std::vector<DisplayLine>& outLines, PageAnchor& next);
  // Wraps one parsed block into display lines. `skipLines` drops leading lines
  // already shown; `emitted` reports how many lines the block produced in total.
  void layoutBlock(GfxRenderer& renderer, const markdown::Block& block, bool firstOnPage, uint16_t skipLines,
                   int availableHeight, std::vector<DisplayLine>& outLines, int& usedHeight, uint16_t& emitted,
                   bool& complete) const;

  int lineTotalHeight(const DisplayLine& line) const;
  // Draws the page. Decorations (rules, quote/code bars, code-span shading) are
  // B/W-only furniture, so the grayscale anti-aliasing pass asks for text alone.
  void drawLines(GfxRenderer& renderer, bool withDecorations) const;
  void renderPage(GfxRenderer& renderer);
  void renderStatusBar() const;

  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  std::string getBookTitle() const override { return doc ? doc->getTitle() : ""; }
  void renderBook() override;

 public:
  explicit MarkdownReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                  bool allowFastInitialRefresh)
      : ReaderActivity("MarkdownReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~MarkdownReaderActivity() override = default;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
