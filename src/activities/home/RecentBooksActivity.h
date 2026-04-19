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
  enum class ContextAction {
    Open,
    ToggleReadMark,
    ResetProgress,
    BookInfo,
    RemoveFromLibrary,
    Cancel,
  };

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  size_t contextSelectedIndex = 0;
  bool contextMenuOpen = false;
  bool bookInfoOpen = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Data loading
  void loadRecentBooks();
  std::vector<ContextAction> getContextActions() const;
  const char* getContextActionLabel(ContextAction action) const;
  void onContextAction(ContextAction action);
  void resetBookProgress(const std::string& path) const;

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
