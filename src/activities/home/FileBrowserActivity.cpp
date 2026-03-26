#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "../util/ConfirmationActivity.h"
#include "../util/KeyboardEntryActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
          FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
          FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileBrowserActivity::loadPickerFolders() {
  pickerFolders.clear();
  pickerFiles.clear();

  auto root = Storage.open(pickerPath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] != '.') {
      if (file.isDirectory()) {
        pickerFolders.emplace_back(std::string(name) + "/");
      } else {
        pickerFiles.emplace_back(std::string(name));
      }
    }
    file.close();
  }
  root.close();
  sortFileList(pickerFolders);
  sortFileList(pickerFiles);
}

void FileBrowserActivity::performMove(const std::string& destPath) {
  const auto pos = fileToMove.rfind('/');
  const std::string filename = (pos != std::string::npos) ? fileToMove.substr(pos + 1) : fileToMove;

  std::string dest = destPath;
  if (dest.back() != '/') dest += "/";
  dest += filename;

  clearFileMetadata(fileToMove);

  bool ok = Storage.rename(fileToMove.c_str(), dest.c_str());

  if (!ok) {
    // rename() fails across directories on FAT32 — fall back to copy + delete
    LOG_DBG("FileBrowser", "rename failed, trying copy+delete: %s -> %s",
            fileToMove.c_str(), dest.c_str());

    HalFile src, dst;
    ok = Storage.openFileForRead("FileBrowser", fileToMove, src) &&
         Storage.openFileForWrite("FileBrowser", dest, dst);

    if (ok) {
      constexpr size_t BUF_SIZE = 512;
      uint8_t buf[BUF_SIZE];
      bool copyOk = true;
      while (src.available()) {
        const int n = src.read(buf, BUF_SIZE);
        if (n <= 0) break;
        if (dst.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          copyOk = false;
          break;
        }
      }
      src.close();
      dst.close();

      if (copyOk) {
        if (Storage.remove(fileToMove.c_str())) {
          LOG_DBG("FileBrowser", "Copy+delete succeeded: %s -> %s",
                  fileToMove.c_str(), dest.c_str());
          ok = true;
        } else {
          // Copy succeeded but original not removed — clean up the copy
          LOG_ERR("FileBrowser", "Copy ok but delete failed, removing copy: %s", dest.c_str());
          Storage.remove(dest.c_str());
          ok = false;
        }
      } else {
        LOG_ERR("FileBrowser", "Copy failed, removing partial dest: %s", dest.c_str());
        Storage.remove(dest.c_str());
        ok = false;
      }
    } else {
      LOG_ERR("FileBrowser", "Could not open files for copy: %s -> %s",
              fileToMove.c_str(), dest.c_str());
    }
  }

  if (!ok) {
    LOG_ERR("FileBrowser", "Move failed: %s -> %s", fileToMove.c_str(), dest.c_str());
  }

  fileToMove.clear();
  mode = BrowserMode::NORMAL;
  loadFiles();
  if (selectorIndex >= files.size() && !files.empty()) {
    selectorIndex = files.size() - 1;
  }
  requestUpdate(true);
}

void FileBrowserActivity::createFolder(const std::string& name) {
  if (name.empty()) return;

  const std::string sanitized = StringUtils::sanitizeFilename(name);
  if (sanitized.empty()) return;

  std::string path = basepath;
  if (path.back() != '/') path += "/";
  path += sanitized;

  if (Storage.mkdir(path.c_str())) {
    LOG_DBG("FileBrowser", "Created folder: %s", path.c_str());
  } else {
    LOG_ERR("FileBrowser", "Failed to create folder: %s", path.c_str());
  }
  loadFiles();
  requestUpdate(true);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  loadFiles();
  selectorIndex = 0;

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  // ── FOLDER PICKER MODE ────────────────────────────────────────────────────
  if (mode == BrowserMode::PICKING_FOLDER) {
    // Confirm: move the file into the currently displayed folder
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performMove(pickerPath);
      return;
    }

    // Right: navigate into the selected subfolder
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (!pickerFolders.empty()) {
        if (pickerPath.back() != '/') pickerPath += "/";
        const std::string& sel = pickerFolders[pickerIndex];
        pickerPath += sel.substr(0, sel.length() - 1);
        pickerIndex = 0;
        loadPickerFolders();
        requestUpdate();
      }
      return;
    }

    // Back: go up one level, or cancel if already at root
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (pickerPath != "/") {
        const auto slash = pickerPath.find_last_of('/');
        pickerPath = (slash == 0) ? "/" : pickerPath.substr(0, slash);
        pickerIndex = 0;
        loadPickerFolders();
        requestUpdate();
      } else {
        fileToMove.clear();
        mode = BrowserMode::NORMAL;
        requestUpdate();
      }
      return;
    }

    // Up/Down: navigate selectable folders only (files are display-only)
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (!pickerFolders.empty()) {
        pickerIndex = ButtonNavigator::previousIndex(static_cast<int>(pickerIndex),
                                                     static_cast<int>(pickerFolders.size()));
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (!pickerFolders.empty()) {
        pickerIndex = ButtonNavigator::nextIndex(static_cast<int>(pickerIndex),
                                                 static_cast<int>(pickerFolders.size()));
        requestUpdate();
      }
      return;
    }
    return;
  }

  // ── NORMAL MODE ───────────────────────────────────────────────────────────

  // Long-press Back (1 s+) jumps to root
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
      // Long-press Confirm: delete file or folder
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      // Strip trailing '/' from dir entry for the actual path
      const std::string entryName = isDirectory ? entry.substr(0, entry.length() - 1) : entry;
      const std::string fullPath = cleanBasePath + entryName;

      auto handler = [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          bool ok = false;
          if (isDirectory) {
            ok = Storage.removeDir(fullPath.c_str());
          } else {
            clearFileMetadata(fullPath);
            ok = Storage.remove(fullPath.c_str());
          }
          if (ok) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              selectorIndex = files.size() - 1;
            }
            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entryName), handler);
      return;
    } else {
      // Short-press Confirm: open / navigate
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (consumeBack) {
      consumeBack = false;
      return;
    }
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;
        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();
        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);
        requestUpdate();
      } else {
        onGoHome();
      }
    }
  }

  // Left (bottom button): initiate Move for the selected file
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!files.empty()) {
      const std::string& entry = files[selectorIndex];
      if (entry.back() != '/') {
        std::string cleanBase = basepath;
        if (cleanBase.back() != '/') cleanBase += "/";
        fileToMove = cleanBase + entry;
        pickerPath = "/";
        pickerIndex = 0;
        loadPickerFolders();
        mode = BrowserMode::PICKING_FOLDER;
        requestUpdate();
      }
    }
    return;
  }

  // Right (bottom button): create a new folder in the current directory
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    consumeBack = true;
    auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "New Folder");
    startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        createFolder(std::get<KeyboardResult>(result.data).text);
      }
    });
    return;
  }

  // Up/Down (side buttons): single-item navigation; page-scroll when held
  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!files.empty()) {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!files.empty()) {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    }
    return;
  }
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // ── FOLDER PICKER MODE ────────────────────────────────────────────────────
  if (mode == BrowserMode::PICKING_FOLDER) {
    // Header: show what we're moving
    const auto pos = fileToMove.rfind('/');
    const std::string filename = (pos != std::string::npos) ? fileToMove.substr(pos + 1) : fileToMove;
    // TODO: replace with tr(STR_MOVE) once added to I18n
    const std::string heading = std::string("Move: ") + filename;
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, heading.c_str());

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    // Current picker path as sub-label
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop,
                      pickerPath.c_str());

    const int listTop = contentTop + 20;
    const int listHeight = contentHeight - 20;

    // Combined list: folders first (selectable), then files (grayed out, context only)
    const size_t totalItems = pickerFolders.size() + pickerFiles.size();
    if (totalItems == 0) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, listTop + 20, tr(STR_NO_FILES_FOUND));
    } else {
      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listHeight}, totalItems, pickerIndex,
          [this](int index) -> std::string {
            if (static_cast<size_t>(index) < pickerFolders.size()) {
              const auto& f = pickerFolders[index];
              return "> " + f.substr(0, f.length() - 1);
            }
            return getFileName(pickerFiles[index - pickerFolders.size()]);
          },
          nullptr,
          [this](int index) {
            if (static_cast<size_t>(index) < pickerFolders.size()) {
              return UITheme::getFileIcon("/");
            }
            return UITheme::getFileIcon(pickerFiles[index - pickerFolders.size()]);
          });
    }

    // TODO: replace "Move Here" / "Enter" with tr() once I18n keys are added
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Move Here",
                                              "", pickerFolders.empty() ? "" : "Enter");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // ── NORMAL MODE ───────────────────────────────────────────────────────────
  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  // Show Move hint only for files (not directories), never when list is empty
  const bool selectedIsFile = !files.empty() && files[selectorIndex].back() != '/';
  const auto labels =
      mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK),
                            files.empty() ? "" : tr(STR_OPEN),
                            selectedIsFile ? "Move" : "",
                            "New Folder");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
