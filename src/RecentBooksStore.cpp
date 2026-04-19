#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 10;

constexpr size_t EPUB_PROGRESS_SIZE = 12;
constexpr size_t XTC_PROGRESS_SIZE = 6;
constexpr size_t TXT_PROGRESS_SIZE = 6;

std::string getProgressFilePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub epub(path, "/.crosspoint");
    return epub.getCachePath() + "/progress.bin";
  }
  if (FsHelpers::hasXtcExtension(path)) {
    Xtc xtc(path, "/.crosspoint");
    return xtc.getCachePath() + "/progress.bin";
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    Txt txt(path, "/.crosspoint");
    return txt.getCachePath() + "/progress.bin";
  }
  return "";
}

uint8_t clampPercent(const int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return static_cast<uint8_t>(value);
}
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, ""});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      recentBooks.push_back({path, title, author, coverBmpPath});
    }

    if (omitted > 0) {
      // Explicitly close() file before saveToFile() rewrites the same file
      inputFile.close();
      saveToFile();
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}

BookProgressStatus RecentBooksStore::getProgressStatus(const std::string& path) const {
  BookProgressStatus status{};
  const std::string progressPath = getProgressFilePath(path);
  if (progressPath.empty()) {
    return status;
  }

  FsFile f;
  if (!Storage.openFileForRead("RBS", progressPath, f)) {
    return status;
  }

  uint8_t data[16] = {0};
  const int readSize = f.read(data, sizeof(data));
  f.close();
  if (readSize <= 0) {
    return status;
  }

  if (FsHelpers::hasEpubExtension(path)) {
    if (readSize >= 11) {
      status.progressPercent = clampPercent(data[10]);
    } else if (readSize >= 6) {
      const int currentPage = data[2] + (data[3] << 8);
      const int pageCount = data[4] + (data[5] << 8);
      status.progressPercent = pageCount > 0 ? clampPercent((currentPage * 100) / pageCount) : 0;
    }
    if (readSize >= 12) {
      status.isMarkedAsRead = data[11] != 0;
    }
  } else if (FsHelpers::hasXtcExtension(path)) {
    if (readSize >= 5) {
      status.progressPercent = clampPercent(data[4]);
    } else if (readSize >= 4) {
      Xtc xtc(path, "/.crosspoint");
      if (xtc.load() && xtc.getPageCount() > 0) {
        const uint32_t currentPage =
            static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
        status.progressPercent = clampPercent(static_cast<int>((currentPage * 100UL) / xtc.getPageCount()));
      }
    }
    if (readSize >= 6) {
      status.isMarkedAsRead = data[5] != 0;
    }
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    if (readSize >= 5) {
      status.progressPercent = clampPercent(data[4]);
    }
    if (readSize >= 6) {
      status.isMarkedAsRead = data[5] != 0;
    }
  }

  return status;
}

bool RecentBooksStore::setMarkedAsRead(const std::string& path, const bool isMarkedAsRead) const {
  const std::string progressPath = getProgressFilePath(path);
  if (progressPath.empty()) {
    return false;
  }

  uint8_t data[16] = {0};
  size_t targetSize = 0;
  if (FsHelpers::hasEpubExtension(path)) {
    targetSize = EPUB_PROGRESS_SIZE;
  } else if (FsHelpers::hasXtcExtension(path)) {
    targetSize = XTC_PROGRESS_SIZE;
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    targetSize = TXT_PROGRESS_SIZE;
  }
  if (targetSize == 0 || targetSize > sizeof(data)) {
    return false;
  }

  FsFile readFile;
  if (Storage.openFileForRead("RBS", progressPath, readFile)) {
    const int readSize = readFile.read(data, targetSize);
    if (readSize > 0 && static_cast<size_t>(readSize) < targetSize) {
      memset(data + readSize, 0, targetSize - static_cast<size_t>(readSize));
    }
    readFile.close();
  }

  if (targetSize == EPUB_PROGRESS_SIZE) {
    data[11] = isMarkedAsRead ? 1 : 0;
  } else {
    data[5] = isMarkedAsRead ? 1 : 0;
  }

  FsFile writeFile;
  if (!Storage.openFileForWrite("RBS", progressPath, writeFile)) {
    return false;
  }
  writeFile.write(data, targetSize);
  writeFile.close();
  return true;
}

bool RecentBooksStore::resetProgress(const std::string& path) const {
  const std::string progressPath = getProgressFilePath(path);
  if (progressPath.empty()) {
    return false;
  }

  uint8_t data[16] = {0};
  size_t targetSize = 0;

  if (FsHelpers::hasEpubExtension(path)) {
    targetSize = EPUB_PROGRESS_SIZE;
  } else if (FsHelpers::hasXtcExtension(path)) {
    targetSize = XTC_PROGRESS_SIZE;
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    targetSize = TXT_PROGRESS_SIZE;
  }
  if (targetSize == 0 || targetSize > sizeof(data)) {
    return false;
  }

  FsFile writeFile;
  if (!Storage.openFileForWrite("RBS", progressPath, writeFile)) {
    return false;
  }
  writeFile.write(data, targetSize);
  writeFile.close();
  return true;
}
