#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class BrowserMode {
    NORMAL,         // Standard file browsing
    PICKING_FOLDER  // Selecting destination folder for a move operation
  };

  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;

  // Move / folder-picker state
  BrowserMode mode = BrowserMode::NORMAL;
  std::string fileToMove;
  std::string pickerPath;
  std::vector<std::string> pickerFolders;
  std::vector<std::string> pickerFiles;  // shown grayed for context, not selectable
  size_t pickerIndex = 0;
  bool consumeBack = false;  // swallows the Back release that dismissed the keyboard

  // Data loading
  void loadFiles();
  void loadPickerFolders();
  size_t findEntry(const std::string& name) const;

  // Actions
  void clearFileMetadata(const std::string& fullPath);
  void performMove(const std::string& destPath);
  void createFolder(const std::string& name);
};
