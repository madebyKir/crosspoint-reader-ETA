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
  static constexpr unsigned long LONG_PRESS_MS = 1000;
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool contextMenuOpen = false;
  bool lockConfirmLongPress = false;
  int contextMenuIndex = 0;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Data loading
  void loadRecentBooks();
  std::string getBookProgressStatus(const RecentBook& book) const;
  void openContextMenu();
  void closeContextMenu();
  void confirmContextMenuAction();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
