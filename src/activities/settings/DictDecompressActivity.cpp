#include "DictDecompressActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>

#include <memory>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// uzlib read callback context
//
// InflateReader MUST be the first member so that the uzlib_uncomp* pointer
// received in the callback can be cast to DecompCtx* (reader.decomp is at
// offset 0 within InflateReader, which is at offset 0 within DecompCtx).
// HalFile is stored as a pointer (non-owning) to keep DecompCtx standard-layout,
// matching the pattern in InflateReader.h's class-level comment.
// ---------------------------------------------------------------------------
struct DecompCtx {
  InflateReader reader;  // MUST be first
  HalFile* file;         // non-owning; caller owns the HalFile instance
  uint8_t chunkBuf[512];
};

static int dictDecompReadCallback(struct uzlib_uncomp* u) {
  DecompCtx* ctx = reinterpret_cast<DecompCtx*>(u);
  int n = ctx->file->read(ctx->chunkBuf, sizeof(ctx->chunkBuf));
  if (n <= 0) return -1;
  u->source = reinterpret_cast<const unsigned char*>(ctx->chunkBuf + 1);
  u->source_limit = reinterpret_cast<const unsigned char*>(ctx->chunkBuf + n);
  return static_cast<int>(ctx->chunkBuf[0]);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictDecompressActivity::onEnter() {
  Activity::onEnter();
  state = CONFIRM;
  decompressProgress = 0;
  decompressTotalSize = 0;
  extractionDone = false;
  extractionSucceeded = false;
  extractTaskHandle = nullptr;
  requestUpdate();
}

void DictDecompressActivity::onExit() {
  // The task only runs during EXTRACTING and always completes before the user
  // can reach SUCCESS/FAILED and press Back. Guard defensively anyway.
  if (extractTaskHandle != nullptr) {
    vTaskDelete(extractTaskHandle);
    extractTaskHandle = nullptr;
  }
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// FreeRTOS task entry
// ---------------------------------------------------------------------------

void DictDecompressActivity::extractTaskEntry(void* param) {
  DictDecompressActivity* self = static_cast<DictDecompressActivity*>(param);
  self->extract();
  self->extractTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Input / state machine
// ---------------------------------------------------------------------------

void DictDecompressActivity::loop() {
  if (state == CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = EXTRACTING;
      extractionDone = false;
      extractionSucceeded = false;
      requestUpdateAndWait();  // show "Extracting..." screen before task starts
      xTaskCreate(extractTaskEntry, "DictDecomp", 4096, this, 1, &extractTaskHandle);
      return;
    }
    return;
  }

  if (state == EXTRACTING) {
    // Poll completion flag set by the extract task.
    if (extractionDone) {
      state = extractionSucceeded ? SUCCESS : FAILED;
      requestUpdate();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (state == FAILED) {
        ActivityResult r;
        r.isCancelled = true;
        setResult(std::move(r));
      }
      // SUCCESS: default result (isCancelled = false) signals the caller to apply selection.
      finish();
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Extraction (runs on FreeRTOS task — NOT on the main/loop task)
// ---------------------------------------------------------------------------

void DictDecompressActivity::extract() {
  char dzPath[520];
  char dictPath[520];
  snprintf(dzPath, sizeof(dzPath), "%s.dict.dz", folderPath.c_str());
  snprintf(dictPath, sizeof(dictPath), "%s.dict", folderPath.c_str());

  // HalFile lives here (owns the file handle). DecompCtx holds a pointer to it.
  HalFile inputFile;

  // DecompCtx on heap: reader (standard-layout) + pointer + 512-byte chunk buffer.
  std::unique_ptr<DecompCtx> ctx(new DecompCtx{});
  ctx->file = &inputFile;

  // Output buffer — 4 KB. Too large for stack (> 256-byte stack limit per CLAUDE.md).
  constexpr size_t OUT_BUF_SIZE = 4096;
  std::unique_ptr<uint8_t[]> outBuf(new uint8_t[OUT_BUF_SIZE]);

  auto fail = [&] {
    inputFile.close();
    extractionSucceeded = false;
    extractionDone = true;
    requestUpdate(true);
  };

  // --- Open input file ---
  if (!Storage.openFileForRead("DICT_DZ", dzPath, inputFile)) {
    LOG_ERR("DICT_DZ", "Failed to open .dict.dz: %s", dzPath);
    fail();
    return;
  }

  decompressTotalSize = inputFile.fileSize();

  // --- Validate gzip magic bytes (0x1F 0x8B) ---
  uint8_t magic[2];
  if (inputFile.read(magic, 2) != 2 || magic[0] != 0x1F || magic[1] != 0x8B) {
    LOG_ERR("DICT_DZ", "Not a gzip file: %s", dzPath);
    fail();
    return;
  }
  inputFile.seekSet(0);  // rewind so uzlib_gzip_parse_header reads from the start

  // --- Init InflateReader in streaming mode ---
  if (!ctx->reader.init(true)) {
    LOG_ERR("DICT_DZ", "InflateReader init failed (out of memory)");
    fail();
    return;
  }

  ctx->reader.setReadCallback(dictDecompReadCallback);

  // --- Parse gzip header ---
  if (!ctx->reader.skipGzipHeader()) {
    LOG_ERR("DICT_DZ", "Invalid gzip header in: %s", dzPath);
    fail();
    return;
  }

  // --- Open output file ---
  HalFile outFile;
  if (!Storage.openFileForWrite("DICT_DZ", dictPath, outFile)) {
    LOG_ERR("DICT_DZ", "Failed to open .dict for write: %s", dictPath);
    fail();
    return;
  }

  // --- Decompress loop ---
  constexpr size_t PROGRESS_INTERVAL = 65536;  // update display every ~64 KB of input consumed
  size_t lastProgressPos = 0;
  InflateStatus status;
  bool writeError = false;

  do {
    size_t produced;
    status = ctx->reader.readAtMost(outBuf.get(), OUT_BUF_SIZE, &produced);

    if (produced > 0) {
      if (outFile.write(outBuf.get(), produced) != produced) {
        LOG_ERR("DICT_DZ", "Write error to: %s", dictPath);
        writeError = true;
        break;
      }
    }

    // Periodic progress update — yield so the render task can repaint.
    const size_t pos = inputFile.position();
    if (pos - lastProgressPos >= PROGRESS_INTERVAL) {
      lastProgressPos = pos;
      decompressProgress = pos;
      requestUpdate(true);
      vTaskDelay(1);
    }
  } while (status == InflateStatus::Ok);

  outFile.close();
  inputFile.close();

  if (writeError || status == InflateStatus::Error) {
    Storage.remove(dictPath);  // clean up partial output
    LOG_ERR("DICT_DZ", "Extraction failed");
    extractionSucceeded = false;
  } else {
    LOG_DBG("DICT_DZ", "Extraction complete: %s", dictPath);
    extractionSucceeded = true;
  }

  extractionDone = true;
  requestUpdate(true);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DictDecompressActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_DICT_EXTRACT_TITLE));

  if (state == CONFIRM) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_DICT_DECOMPRESS_WARN_1));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_DICT_DECOMPRESS_WARN_2));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == EXTRACTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DICT_EXTRACTING));

    if (decompressTotalSize > 0) {
      constexpr int BAR_HEIGHT = 20;
      constexpr int BAR_MARGIN = 60;
      const int barY = pageHeight / 2;
      GUI.drawProgressBar(renderer, Rect{BAR_MARGIN, barY, pageWidth - BAR_MARGIN * 2, BAR_HEIGHT},
                          decompressProgress, decompressTotalSize);
    }

    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_DICT_EXTRACT_SUCCESS), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_DICT_EXTRACT_FAILED), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}
