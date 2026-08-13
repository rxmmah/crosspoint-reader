#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiListActivity.h"

class RecentBooksActivity final : public UiListActivity {
 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               HomeMenuItem homeReturnItemValue = HomeMenuItem::NONE,
                               int homeReturnRecentIndexValue = -1);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(recentBooks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Confirm activates on RELEASE here (a hold is "remove from list"), and Back
  // goes home rather than finishing.
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_MENU_RECENT_BOOKS); }
  void drawFooter() override;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // True after the ConfirmationActivity pushed by promptRemoveBook closed via a physical Back
  // press (to cancel): that press's release is still pending and must not be reinterpreted as
  // our own Back release, which would immediately go home on top of just closing the dialog.
  bool lockNextBackRelease = false;

  std::vector<RecentBook> recentBooks;
  // Row buffer, built in loadRecentBooks() (not buildScreen(), which reuses
  // it on every repaint instead of rebuilding a ListItem vector per render).
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();

  // Where to put the home selector when Back returns home. Set when this
  // activity was opened by the home Back shortcut, which must leave the home
  // selection where the user left it instead of moving it onto "Recent Books".
  const HomeMenuItem homeReturnItem;
  const int homeReturnRecentIndex;

  // Data loading
  void loadRecentBooks();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);
};
