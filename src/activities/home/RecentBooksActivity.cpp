#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int MENU_ITEM_COUNT = 2;
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  progressStatuses.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  progressStatuses.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
    progressStatuses.push_back(RECENT_BOOKS.getProgressStatus(book.path));
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  contextMenuOpen = false;
  contextMenuIndex = 0;
  ignoreConfirmUntilRelease = false;
  lockLongPressConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  progressStatuses.clear();
}

void RecentBooksActivity::openContextMenu() {
  contextMenuOpen = true;
  contextMenuIndex = 0;
  ignoreConfirmUntilRelease = true;
  requestUpdate();
}

void RecentBooksActivity::closeContextMenu() {
  contextMenuOpen = false;
  contextMenuIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::applyContextAction() {
  if (recentBooks.empty() || selectorIndex >= recentBooks.size() || selectorIndex >= progressStatuses.size()) {
    closeContextMenu();
    return;
  }

  auto& status = progressStatuses[selectorIndex];
  const auto& book = recentBooks[selectorIndex];
  if (contextMenuIndex == 0) {
    const bool nextMarked = !status.isMarkedAsRead;
    if (RECENT_BOOKS.setMarkedAsRead(book.path, nextMarked)) {
      status.isMarkedAsRead = nextMarked;
    }
  } else if (contextMenuIndex == 1) {
    if (RECENT_BOOKS.resetProgress(book.path)) {
      status.progressPercent = 0;
      status.isMarkedAsRead = false;
    }
  }
  closeContextMenu();
}

std::string RecentBooksActivity::getProgressText(const size_t index) const {
  if (index >= progressStatuses.size()) {
    return "0%";
  }
  const auto& status = progressStatuses[index];
  if (status.isMarkedAsRead || status.progressPercent >= 100) {
    return tr(STR_READ_COMPLETED);
  }
  return std::to_string(status.progressPercent) + "%";
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (lockLongPressConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockLongPressConfirm = false;
    return;
  }

  if (contextMenuOpen && ignoreConfirmUntilRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmUntilRelease = false;
    }
    return;
  }

  if (!contextMenuOpen && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && !recentBooks.empty()) {
    openContextMenu();
    return;
  }

  if (contextMenuOpen) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeContextMenu();
      return;
    }

    buttonNavigator.onNextRelease([this] {
      contextMenuIndex = (contextMenuIndex + 1) % MENU_ITEM_COUNT;
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      contextMenuIndex = (contextMenuIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      applyContextAction();
      return;
    }

    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS && !recentBooks.empty() &&
        selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); },
        [this](int index) { return getProgressText(static_cast<size_t>(index)); }, false);

  }

  if (contextMenuOpen && !recentBooks.empty() && selectorIndex < recentBooks.size()) {
    const char* menuItems[MENU_ITEM_COUNT] = {
        progressStatuses[selectorIndex].isMarkedAsRead ? tr(STR_UNMARK_AS_READ) : tr(STR_MARK_AS_READ),
        tr(STR_RESET_PROGRESS)};

    constexpr int popupWidth = 320;
    constexpr int rowHeight = 28;
    constexpr int innerPadding = 8;
    constexpr int estimatedRowHeight = 72; 

    const int popupHeight = MENU_ITEM_COUNT * rowHeight + innerPadding * 2;

    int popupX = pageWidth - popupWidth - metrics.contentSidePadding;
    int popupY = contentTop + static_cast<int>(selectorIndex) * estimatedRowHeight + 6;

    const int minPopupY = contentTop;
    const int maxPopupY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - popupHeight;

    if (popupY < minPopupY) popupY = minPopupY;
    if (popupY > maxPopupY) popupY = maxPopupY;

    renderer.fillRect(popupX, popupY, popupWidth, popupHeight, false);
    renderer.drawRect(popupX, popupY, popupWidth, popupHeight, true);

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
      const int rowY = popupY + innerPadding + i * rowHeight;
      const bool selected = (contextMenuIndex == static_cast<size_t>(i));

      const int highlightY = rowY + 2;
      const int highlightHeight = rowHeight - 4;

      if (selected) {
        renderer.fillRect(popupX + innerPadding,
                          highlightY,
                          popupWidth - innerPadding * 2,
                          highlightHeight,
                          true);
      }

      renderer.drawText(UI_10_FONT_ID,
                        popupX + innerPadding + 8,
                        rowY + (rowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                        menuItems[i],
                        !selected);
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels(contextMenuOpen ? tr(STR_BACK) : tr(STR_HOME),
                                            contextMenuOpen ? tr(STR_SELECT) : tr(STR_OPEN), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
