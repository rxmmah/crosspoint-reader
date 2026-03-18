#pragma once
#include <EpdFontFamily.h>

#include <string>
#include <vector>

#include "../Activity.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  // showDoneButton: if true, Confirm acts as "Done" (exit to reader); otherwise Back/Confirm both return to caller
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::string& headword, const std::string& definition, int readerFontId,
                                        bool showDoneButton = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        definition(definition),
        readerFontId(readerFontId),
        showDoneButton(showDoneButton) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string headword;
  std::string definition;
  int readerFontId;
  bool showDoneButton;

  // A single styled run within a display line.
  struct LayoutSegment {
    std::string text;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };

  // One wrapped display line, containing one or more styled segments.
  struct LayoutLine {
    std::vector<LayoutSegment> segments;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  std::vector<LayoutLine> layoutLines;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;

  // Orientation-aware layout gutters (computed in wrapText, used in render)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;

  void wrapText();
  void wrapHtml();
  void wrapPlain();
};
