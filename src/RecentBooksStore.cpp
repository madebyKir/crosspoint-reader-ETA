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

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 10;

uint8_t clampPercent(int percent) { return static_cast<uint8_t>(std::max(0, std::min(100, percent))); }

uint8_t readEpubProgressPercent(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return 0;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBS", epub.getCachePath() + "/progress.bin", file)) {
    return 0;
  }

  uint8_t data[10] = {0};
  const int readSize = file.read(data, sizeof(data));
  file.close();
  if (readSize < 6) {
    return 0;
  }

  const int spineIndex = data[0] | (data[1] << 8);
  const int currentPage = data[2] | (data[3] << 8);
  const int pageCount = std::max(1, data[4] | (data[5] << 8));
  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  const float progress = epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
  return clampPercent(static_cast<int>(progress + 0.5f));
}

bool readTxtTotalPages(const std::string& indexPath, uint32_t& totalPages) {
  constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
  FsFile f;
  if (!Storage.openFileForRead("RBS", indexPath, f)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t cachedWidth = 0;
  int32_t cachedLines = 0;
  int32_t fontId = 0;
  int32_t margin = 0;
  uint8_t alignment = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, fileSize);
  serialization::readPod(f, cachedWidth);
  serialization::readPod(f, cachedLines);
  serialization::readPod(f, fontId);
  serialization::readPod(f, margin);
  serialization::readPod(f, alignment);
  serialization::readPod(f, totalPages);
  f.close();

  return magic == CACHE_MAGIC && totalPages > 0;
}

uint8_t readTxtProgressPercent(const std::string& path) {
  Txt txt(path, "/.crosspoint");

  FsFile f;
  if (!Storage.openFileForRead("RBS", txt.getCachePath() + "/progress.bin", f)) {
    return 0;
  }
  uint8_t data[4] = {0};
  if (f.read(data, sizeof(data)) != sizeof(data)) {
    f.close();
    return 0;
  }
  f.close();

  const uint32_t currentPage = data[0] | (data[1] << 8);
  uint32_t totalPages = 0;
  if (!readTxtTotalPages(txt.getCachePath() + "/index.bin", totalPages)) {
    return 0;
  }

  const int progress = static_cast<int>((currentPage + 1) * 100U / totalPages);
  return clampPercent(progress);
}

uint8_t readXtcProgressPercent(const std::string& path) {
  Xtc xtc(path, "/.crosspoint");
  if (!xtc.load()) {
    return 0;
  }

  FsFile f;
  if (!Storage.openFileForRead("RBS", xtc.getCachePath() + "/progress.bin", f)) {
    return 0;
  }

  uint8_t data[4] = {0};
  if (f.read(data, sizeof(data)) != sizeof(data)) {
    f.close();
    return 0;
  }
  f.close();

  const uint32_t currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
  return clampPercent(xtc.calculateProgress(currentPage));
}

uint8_t readProgressPercentFromStorage(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return readEpubProgressPercent(path);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return readXtcProgressPercent(path);
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return readTxtProgressPercent(path);
  }
  return 0;
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
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath, 0, false});

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

bool RecentBooksStore::updateReadingProgress(const std::string& path, uint8_t progressPercent) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }

  const uint8_t clampedPercent = clampPercent(progressPercent);
  if (it->progressPercent == clampedPercent) {
    return true;
  }

  it->progressPercent = clampedPercent;
  return saveToFile();
}

bool RecentBooksStore::setMarkedAsRead(const std::string& path, bool isMarkedAsRead) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }

  if (it->isMarkedAsRead == isMarkedAsRead) {
    return true;
  }

  it->isMarkedAsRead = isMarkedAsRead;
  return saveToFile();
}

bool RecentBooksStore::resetReadingProgress(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }

  it->progressPercent = 0;
  it->isMarkedAsRead = false;

  if (FsHelpers::hasEpubExtension(path)) {
    const std::string cachePath = Epub(path, "/.crosspoint").getCachePath() + "/progress.bin";
    if (Storage.exists(cachePath.c_str())) {
      Storage.remove(cachePath.c_str());
    }
  } else if (FsHelpers::hasXtcExtension(path)) {
    const std::string cachePath = Xtc(path, "/.crosspoint").getCachePath() + "/progress.bin";
    if (Storage.exists(cachePath.c_str())) {
      Storage.remove(cachePath.c_str());
    }
  } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    const std::string cachePath = Txt(path, "/.crosspoint").getCachePath() + "/progress.bin";
    if (Storage.exists(cachePath.c_str())) {
      Storage.remove(cachePath.c_str());
    }
  }

  return saveToFile();
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
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath(), 0, false};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath(), 0, false};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", "", 0, false};
  }
  return RecentBook{path, "", "", "", 0, false};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      if (!JsonSettingsIO::loadRecentBooks(*this, json.c_str())) {
        return false;
      }

      bool needsSave = false;
      for (auto& book : recentBooks) {
        if (book.progressPercent == 0 && !book.isMarkedAsRead) {
          const auto scannedProgress = readProgressPercentFromStorage(book.path);
          if (scannedProgress > 0) {
            book.progressPercent = scannedProgress;
            needsSave = true;
          }
        }
      }

      if (needsSave) {
        saveToFile();
      }
      return true;
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
        recentBooks.push_back({path, title, author, "", 0, false});
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

      recentBooks.push_back({path, title, author, coverBmpPath, 0, false});
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

  for (auto& book : recentBooks) {
    book.progressPercent = readProgressPercentFromStorage(book.path);
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
