#include "RecentBooksActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

constexpr int CONTEXT_MENU_WIDTH = 300;
constexpr int CONTEXT_MENU_ROW_HEIGHT = 34;
constexpr int CONTEXT_MENU_INNER_PADDING = 8;
constexpr int CONTEXT_MENU_TOP_MARGIN = 28;
constexpr int CONTEXT_MENU_BOTTOM_MARGIN = 28;

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  contextMenuOpen = false;
  bookInfoOpen = false;
  contextSelectedIndex = 0;
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (bookInfoOpen) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      bookInfoOpen = false;
      requestUpdate();
    }
    return;
  }

  if (contextMenuOpen) {
    const auto actions = getContextActions();
    const int actionCount = static_cast<int>(actions.size());

    buttonNavigator.onNextRelease([this, actionCount] {
      contextSelectedIndex = ButtonNavigator::nextIndex(static_cast<int>(contextSelectedIndex), actionCount);
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this, actionCount] {
      contextSelectedIndex = ButtonNavigator::previousIndex(static_cast<int>(contextSelectedIndex), actionCount);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onContextAction(actions[contextSelectedIndex]);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      contextMenuOpen = false;
      requestUpdate();
      return;
    }

    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      if (mappedInput.getHeldTime() >= GO_HOME_MS) {
        contextMenuOpen = true;
        contextSelectedIndex = 0;
        requestUpdate();
      } else {
        LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
        onSelectBook(recentBooks[selectorIndex].path);
      }
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

std::vector<RecentBooksActivity::ContextAction> RecentBooksActivity::getContextActions() const {
  if (recentBooks.empty() || selectorIndex >= recentBooks.size()) {
    return {ContextAction::Cancel};
  }

  return {ContextAction::Open,      ContextAction::ToggleReadMark, ContextAction::ResetProgress,
          ContextAction::BookInfo,  ContextAction::RemoveFromLibrary, ContextAction::Cancel};
}

const char* RecentBooksActivity::getContextActionLabel(const ContextAction action) const {
  if (action == ContextAction::Open) return tr(STR_OPEN);
  if (action == ContextAction::ToggleReadMark) {
    const bool isRead =
        !recentBooks.empty() && selectorIndex < recentBooks.size() && recentBooks[selectorIndex].isMarkedAsRead;
    return isRead ? tr(STR_UNMARK_AS_READ) : tr(STR_MARK_AS_READ);
  }
  if (action == ContextAction::ResetProgress) return tr(STR_RESET_PROGRESS);
  if (action == ContextAction::BookInfo) return tr(STR_BOOK_INFO);
  if (action == ContextAction::RemoveFromLibrary) return tr(STR_REMOVE_FROM_LIBRARY);
  return tr(STR_CANCEL);
}

int RecentBooksActivity::getBookProgressPercent(const RecentBook& book) const {
  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (!epub.load(false, true)) {
      return 0;
    }

    FsFile progressFile;
    if (!Storage.openFileForRead("RBA", epub.getCachePath() + "/progress.bin", progressFile)) {
      return 0;
    }

    uint8_t progressData[6];
    if (progressFile.read(progressData, sizeof(progressData)) != sizeof(progressData)) {
      return 0;
    }

    const int currentSpineIndex = static_cast<int>(progressData[0] | (progressData[1] << 8));
    const int currentPage = static_cast<int>(progressData[2] | (progressData[3] << 8));
    const int pageCount = static_cast<int>(progressData[4] | (progressData[5] << 8));
    if (currentSpineIndex < 0 || currentSpineIndex >= epub.getSpineItemsCount() || pageCount <= 0) {
      return 0;
    }

    const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
    const float progress = epub.calculateProgress(currentSpineIndex, chapterProgress);
    return std::clamp(static_cast<int>(progress * 100.0f), 0, 100);
  }

  if (FsHelpers::hasXtcExtension(book.path)) {
    Xtc xtc(book.path, "/.crosspoint");
    if (!xtc.load()) {
      return 0;
    }

    FsFile progressFile;
    if (!Storage.openFileForRead("RBA", xtc.getCachePath() + "/progress.bin", progressFile)) {
      return 0;
    }

    uint8_t progressData[4];
    if (progressFile.read(progressData, sizeof(progressData)) != sizeof(progressData)) {
      return 0;
    }

    const uint32_t currentPage = readLe32(progressData);
    return static_cast<int>(xtc.calculateProgress(currentPage));
  }

  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    Txt txt(book.path, "/.crosspoint");
    FsFile progressFile;
    if (!Storage.openFileForRead("RBA", txt.getCachePath() + "/progress.bin", progressFile)) {
      return 0;
    }

    uint8_t progressData[2];
    if (progressFile.read(progressData, sizeof(progressData)) != sizeof(progressData)) {
      return 0;
    }
    const int currentPage = static_cast<int>(progressData[0] | (progressData[1] << 8));

    FsFile indexFile;
    if (!Storage.openFileForRead("RBA", txt.getCachePath() + "/index.bin", indexFile)) {
      return 0;
    }

    uint8_t header[30];
    if (indexFile.read(header, sizeof(header)) != sizeof(header)) {
      return 0;
    }

    const uint32_t magic = readLe32(header);
    constexpr uint32_t TXTI_MAGIC = 0x49545854;  // "TXTI"
    if (magic != TXTI_MAGIC) {
      return 0;
    }

    const uint32_t totalPages = readLe32(header + 26);
    if (totalPages == 0) {
      return 0;
    }
    return std::clamp((currentPage + 1) * 100 / static_cast<int>(totalPages), 0, 100);
  }

  return 0;
}

std::string RecentBooksActivity::getBookProgressLabel(const RecentBook& book) const {
  const int progress = getBookProgressPercent(book);
  if (book.isMarkedAsRead || progress >= 100) {
    return tr(STR_READ_COMPLETED);
  }
  return std::to_string(progress) + "%";
}

void RecentBooksActivity::resetBookProgress(const std::string& path) const {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
    return;
  }

  if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
    return;
  }

  // TODO: add explicit progress reset for TXT/MD once a dedicated API is available.
  LOG_DBG("RBA", "No explicit progress reset implementation for: %s", path.c_str());
}

void RecentBooksActivity::onContextAction(const ContextAction action) {
  if (recentBooks.empty() || selectorIndex >= recentBooks.size()) {
    contextMenuOpen = false;
    requestUpdate();
    return;
  }

  RecentBook& selectedBook = recentBooks[selectorIndex];

  switch (action) {
    case ContextAction::Open:
      contextMenuOpen = false;
      onSelectBook(selectedBook.path);
      return;
    case ContextAction::ToggleReadMark: {
      const bool newReadState = !selectedBook.isMarkedAsRead;
      selectedBook.isMarkedAsRead = newReadState;
      RECENT_BOOKS.setBookRead(selectedBook.path, newReadState);
      contextMenuOpen = false;
      requestUpdate();
      return;
    }
    case ContextAction::ResetProgress:
      resetBookProgress(selectedBook.path);
      selectedBook.isMarkedAsRead = false;
      RECENT_BOOKS.setBookRead(selectedBook.path, false);
      contextMenuOpen = false;
      requestUpdate();
      return;
    case ContextAction::BookInfo:
      contextMenuOpen = false;
      bookInfoOpen = true;
      requestUpdate();
      return;
    case ContextAction::RemoveFromLibrary:
      RECENT_BOOKS.removeBook(selectedBook.path);
      loadRecentBooks();
      if (!recentBooks.empty() && selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      contextMenuOpen = false;
      requestUpdate();
      return;
    case ContextAction::Cancel:
      contextMenuOpen = false;
      requestUpdate();
      return;
  }
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
        [this](int index) { return getBookProgressLabel(recentBooks[index]); });
  }

  if (contextMenuOpen) {
    const auto actions = getContextActions();
    const int menuItemCount = static_cast<int>(actions.size());
    const int menuHeight = menuItemCount * CONTEXT_MENU_ROW_HEIGHT + CONTEXT_MENU_INNER_PADDING * 2;
    const int menuWidth = std::min(CONTEXT_MENU_WIDTH, pageWidth - metrics.contentSidePadding * 2);
    const int menuX = (pageWidth - menuWidth) / 2;
    const int maxY = pageHeight - menuHeight - CONTEXT_MENU_BOTTOM_MARGIN;
    const int menuY = std::max(CONTEXT_MENU_TOP_MARGIN, maxY / 2);

    renderer.fillRect(menuX, menuY, menuWidth, menuHeight, false);
    renderer.drawRect(menuX, menuY, menuWidth, menuHeight, true);

    for (int i = 0; i < menuItemCount; i++) {
      const int rowY = menuY + CONTEXT_MENU_INNER_PADDING + i * CONTEXT_MENU_ROW_HEIGHT;
      const bool selected = i == static_cast<int>(contextSelectedIndex);
      renderer.fillRect(menuX + CONTEXT_MENU_INNER_PADDING, rowY, menuWidth - CONTEXT_MENU_INNER_PADDING * 2,
                        CONTEXT_MENU_ROW_HEIGHT, selected);
      renderer.drawText(UI_10_FONT_ID, menuX + CONTEXT_MENU_INNER_PADDING + 8,
                        rowY + (CONTEXT_MENU_ROW_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                        getContextActionLabel(actions[i]), !selected);
    }
  }

  if (bookInfoOpen && !recentBooks.empty() && selectorIndex < recentBooks.size()) {
    const RecentBook& selectedBook = recentBooks[selectorIndex];
    const int popupWidth = pageWidth - metrics.contentSidePadding * 2;
    const int popupHeight = 144;
    const int popupX = metrics.contentSidePadding;
    const int popupY = (pageHeight - popupHeight) / 2;
    const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + 2;

    renderer.fillRect(popupX, popupY, popupWidth, popupHeight, false);
    renderer.drawRect(popupX, popupY, popupWidth, popupHeight, true);

    renderer.drawText(UI_10_FONT_ID, popupX + 8, popupY + 8, tr(STR_BOOK_INFO), true);
    renderer.drawText(SMALL_FONT_ID, popupX + 8, popupY + 8 + lineStep, selectedBook.title.c_str(), true);
    renderer.drawText(SMALL_FONT_ID, popupX + 8, popupY + 8 + lineStep * 2, selectedBook.author.c_str(), true);
    renderer.drawText(SMALL_FONT_ID, popupX + 8, popupY + 8 + lineStep * 3, selectedBook.path.c_str(), true);
  }

  // Help text
  const auto labels = contextMenuOpen
                          ? mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : bookInfoOpen ? mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "")
                                         : mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP),
                                                                 tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
