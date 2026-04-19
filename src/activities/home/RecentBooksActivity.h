#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<BookProgressStatus> progressStatuses;
  bool contextMenuOpen = false;
  size_t contextMenuIndex = 0;
  bool ignoreConfirmUntilRelease = false;
  bool lockLongPressConfirm = false;

  // Data loading
  void loadRecentBooks();
  void openContextMenu();
  void closeContextMenu();
  void applyContextAction();
  std::string getProgressText(size_t index) const;

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
