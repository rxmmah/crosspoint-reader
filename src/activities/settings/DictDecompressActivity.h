#pragma once

#include <cstddef>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activities/Activity.h"

// Activity that decompresses a .dict.dz file into a .dict file.
//
// Launched by DictionarySelectActivity when the user selects a compressed dictionary.
// Shows a confirmation screen before starting, then streams decompression on a
// FreeRTOS task so loop() keeps returning — allowing the main loop to poll
// preventAutoSleep() and keep resetting the inactivity timer throughout extraction.
// Sets isCancelled = false on success, true on cancel or failure.
class DictDecompressActivity final : public Activity {
 public:
  explicit DictDecompressActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                  std::string folderPath)
      : Activity("DictDecompress", renderer, mappedInput), folderPath(std::move(folderPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return state == EXTRACTING; }
  bool preventAutoSleep() override { return state == EXTRACTING; }
  void render(RenderLock&&) override;

 private:
  enum State { CONFIRM, EXTRACTING, SUCCESS, FAILED };

  State state = CONFIRM;
  std::string folderPath;

  // Progress updated by the extract task; read by the render task.
  // size_t writes are atomic on 32-bit RISC-V so no mutex needed.
  volatile size_t decompressProgress = 0;
  volatile size_t decompressTotalSize = 0;

  // Completion flags set by the extract task, polled by loop().
  volatile bool extractionDone = false;
  volatile bool extractionSucceeded = false;

  TaskHandle_t extractTaskHandle = nullptr;

  // Entry point for the FreeRTOS extraction task.
  void extract();
  static void extractTaskEntry(void* param);
};
