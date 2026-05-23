#include "Section.h"

#include <Arduino.h>
#include <algorithm>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// Version 41: CSS may be skipped under low heap to keep section builds readable.
constexpr uint8_t SECTION_FILE_VERSION = 41;
// Minimum free heap required before attempting to build section pages.
// Section building involves heavy allocations (Page, TextBlock, PageLine, etc.)
// and on ESP32 without C++ exceptions, allocation failure calls abort().
// Keep small XHTML files usable while still requiring more headroom for larger chapters.
constexpr size_t MIN_FREE_HEAP_FOR_TINY_SECTION_BUILD = 30 * 1024;   // 30KB
constexpr size_t MIN_FREE_HEAP_FOR_SMALL_SECTION_BUILD = 36 * 1024;  // 36KB
constexpr size_t MIN_FREE_HEAP_FOR_MEDIUM_SECTION_BUILD = 48 * 1024; // 48KB
constexpr size_t MIN_FREE_HEAP_FOR_LARGE_SECTION_BUILD = 64 * 1024;  // 64KB
constexpr size_t MIN_FREE_HEAP_AFTER_CSS_LOAD = 64 * 1024;           // 64KB
// ZIP inflate streaming needs a 32KB sliding window plus a little room for file and temp allocations.
constexpr size_t MIN_MAX_ALLOC_FOR_SECTION_STREAM = 30 * 1024;  // 30KB
constexpr size_t MIN_FREE_HEAP_FOR_SECTION_STREAM = 30 * 1024;  // 30KB
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint8_t) +  // charSpacing
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct SectionHeader {
  uint8_t version = 0;
  int fontId = 0;
  float lineCompression = 0.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool firstLineIndent = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool verticalMode = false;
  uint8_t charSpacing = 0;
};

double msToSeconds(const uint32_t elapsedMs) {
  return static_cast<double>(elapsedMs) / 1000.0;
}

bool hasEnoughHeapForSectionStream() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  const bool ok = freeHeap >= MIN_FREE_HEAP_FOR_SECTION_STREAM && maxAllocHeap >= MIN_MAX_ALLOC_FOR_SECTION_STREAM;
  if (!ok) {
    LOG_ERR("SCT", "Insufficient heap for section stream (free=%u, maxAlloc=%u, need free>=%zu maxAlloc>=%zu)",
            freeHeap, maxAllocHeap, MIN_FREE_HEAP_FOR_SECTION_STREAM, MIN_MAX_ALLOC_FOR_SECTION_STREAM);
  }
  return ok;
}

size_t requiredHeapForSectionBuild(const uint32_t htmlSize) {
  if (htmlSize <= 2 * 1024) {
    return MIN_FREE_HEAP_FOR_TINY_SECTION_BUILD;
  }
  if (htmlSize <= 10 * 1024) {
    return MIN_FREE_HEAP_FOR_SMALL_SECTION_BUILD;
  }
  if (htmlSize <= 32 * 1024) {
    return MIN_FREE_HEAP_FOR_MEDIUM_SECTION_BUILD;
  }
  return MIN_FREE_HEAP_FOR_LARGE_SECTION_BUILD;
}

bool readSectionHeader(FsFile& file, SectionHeader& header) {
  serialization::readPod(file, header.version);
  serialization::readPod(file, header.fontId);
  serialization::readPod(file, header.lineCompression);
  serialization::readPod(file, header.extraParagraphSpacing);
  serialization::readPod(file, header.paragraphAlignment);
  serialization::readPod(file, header.viewportWidth);
  serialization::readPod(file, header.viewportHeight);
  serialization::readPod(file, header.hyphenationEnabled);
  serialization::readPod(file, header.firstLineIndent);
  serialization::readPod(file, header.embeddedStyle);
  serialization::readPod(file, header.imageRendering);
  serialization::readPod(file, header.verticalMode);
  serialization::readPod(file, header.charSpacing);
  return true;
}
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool firstLineIndent, const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool verticalMode, const uint8_t charSpacing) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(firstLineIndent) + sizeof(embeddedStyle) + sizeof(imageRendering) +
                                   sizeof(verticalMode) + sizeof(charSpacing) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, firstLineIndent);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, verticalMode);
  serialization::writePod(file, charSpacing);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool firstLineIndent,
                              const bool embeddedStyle, const uint8_t imageRendering, const bool verticalMode,
                              const uint8_t charSpacing) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  SectionHeader header;
  readSectionHeader(file, header);
  if (header.version != SECTION_FILE_VERSION) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: Unknown version %u", header.version);
    clearCache();
    return false;
  }

  if (fontId != header.fontId || lineCompression != header.lineCompression ||
      extraParagraphSpacing != header.extraParagraphSpacing || paragraphAlignment != header.paragraphAlignment ||
      viewportWidth != header.viewportWidth || viewportHeight != header.viewportHeight ||
      hyphenationEnabled != header.hyphenationEnabled || firstLineIndent != header.firstLineIndent ||
      embeddedStyle != header.embeddedStyle || imageRendering != header.imageRendering ||
      verticalMode != header.verticalMode || charSpacing != header.charSpacing) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
    clearCache();
    return false;
  }

  serialization::readPod(file, pageCount);
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

CssParser* Section::loadEmbeddedCssForSection(const bool embeddedStyle) {
  if (!embeddedStyle) {
    return nullptr;
  }

  CssParser* cssParser = epub->getCssParser();
  if (!cssParser) {
    return nullptr;
  }

  if (!cssParser->loadFromCache()) {
    LOG_ERR("SCT", "Failed to load CSS from cache");
    return nullptr;
  }

  if (cssParser->empty()) {
    LOG_DBG("SCT", "CSS cache has no rules, skipping stylesheet lookup for this section");
    cssParser->clear();
    return nullptr;
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP_AFTER_CSS_LOAD) {
    LOG_INF("SCT", "Skipping external CSS for section build (rules=%zu, free=%u, need>=%zu)", cssParser->ruleCount(),
            ESP.getFreeHeap(), MIN_FREE_HEAP_AFTER_CSS_LOAD);
    cssParser->clear();
    return nullptr;
  }

  return cssParser;
}

bool Section::streamSpineItemToTempHtml(const std::string& localPath, const std::string& tmpHtmlPath,
                                        uint32_t& fileSize) {
  // Retry logic for SD card timing issues
  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }

    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  return success;
}

bool Section::readSectionOffsets(FsFile& file, uint32_t& lutOffset, uint32_t& anchorMapOffset) const {
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  serialization::readPod(file, lutOffset);
  serialization::readPod(file, anchorMapOffset);
  return true;
}

bool Section::finalizeSectionFile(const std::vector<uint32_t>& lut,
                                  const std::vector<std::pair<std::string, uint16_t>>& anchors,
                                  const std::string& tmpSectionPath, CssParser* cssParser,
                                  const uint32_t createSectionStart, const uint32_t parseBuildStart) {
  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  const uint32_t anchorMapOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 2 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  file.close();
  if (cssParser) {
    cssParser->clear();
  }

  const uint32_t parseBuildElapsedMs = millis() - parseBuildStart;
  const uint32_t totalElapsedMs = millis() - createSectionStart;
  LOG_DBG("SCT", "Section %d page build took %lu ms (%.2f s)", spineIndex, parseBuildElapsedMs,
          msToSeconds(parseBuildElapsedMs));
  LOG_DBG("SCT", "Section %d total create took %lu ms (%.2f s)", spineIndex, totalElapsedMs,
          msToSeconds(totalElapsedMs));

  if (Storage.exists(filePath.c_str()) && !Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to remove old section cache before rename");
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to finalize section cache: %s -> %s", tmpSectionPath.c_str(), filePath.c_str());
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled,
                                const bool firstLineIndent, const bool embeddedStyle, const uint8_t imageRendering,
                                const bool verticalMode, const uint8_t charSpacing,
                                const std::function<void()>& popupFn, const int* headingFontIds,
                                const int tableFontId,
                                const std::function<void(uint16_t pagesDone, uint16_t estimatedPages)>& progressFn) {
  const uint32_t createSectionStart = millis();
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // ZIP inflation needs a 32KB contiguous buffer. Check this before we spend
  // memory on CSS/cache setup or temp-file retries.
  if (!hasEnoughHeapForSectionStream()) {
    return false;
  }

  bool success = false;
  uint32_t fileSize = 0;
  success = streamSpineItemToTempHtml(localPath, tmpHtmlPath, fileSize);

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  const uint32_t streamElapsedMs = millis() - createSectionStart;
  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes) in %lu ms (%.2f s)", tmpHtmlPath.c_str(), fileSize,
          streamElapsedMs, msToSeconds(streamElapsedMs));

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, firstLineIndent, embeddedStyle, imageRendering,
                         verticalMode, charSpacing);
  std::vector<uint32_t> lut = {};

  CssParser* cssParser = loadEmbeddedCssForSection(embeddedStyle);

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  // Pre-check heap before heavy allocation work.
  // On ESP32 without C++ exceptions, new/make_shared call abort() on failure.
  const uint32_t freeHeapBeforeBuild = ESP.getFreeHeap();
  const size_t requiredHeapBeforeBuild = requiredHeapForSectionBuild(fileSize);
  if (freeHeapBeforeBuild < requiredHeapBeforeBuild) {
    LOG_ERR("SCT", "Insufficient heap for section build (%u bytes free, need %zu, html=%lu), aborting gracefully",
            freeHeapBeforeBuild, requiredHeapBeforeBuild, static_cast<unsigned long>(fileSize));
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpHtmlPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  LOG_DBG("SCT", "Section build heap check passed (free=%u, need=%zu, html=%lu)", freeHeapBeforeBuild,
          requiredHeapBeforeBuild, static_cast<unsigned long>(fileSize));

  const uint32_t parseBuildStart = millis();
  const uint32_t estimatedBytesPerPage = verticalMode ? 700 : 3072;
  const uint16_t estimatedPages =
      std::max<uint16_t>(4, static_cast<uint16_t>((fileSize + estimatedBytesPerPage - 1) / estimatedBytesPerPage));
  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, firstLineIndent,
      [this, &lut, &progressFn, estimatedPages](std::unique_ptr<Page> page) {
        lut.emplace_back(this->onPageComplete(std::move(page)));
        if (progressFn) {
          progressFn(pageCount, estimatedPages);
        }
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser, headingFontIds, tableFontId,
      verticalMode);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const auto& anchors = visitor.getAnchors();
  return finalizeSectionFile(lut, anchors, tmpSectionPath, cssParser, createSectionStart, parseBuildStart);
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  return loadPageFromSectionFile(currentPage);
}

std::unique_ptr<Page> Section::loadPageFromSectionFile(const uint16_t pageNumber) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  if (pageNumber >= pageCount) {
    file.close();
    return nullptr;
  }

  uint32_t lutOffset = 0;
  uint32_t anchorMapOffset = 0;
  readSectionOffsets(file, lutOffset, anchorMapOffset);
  file.seek(lutOffset + sizeof(uint32_t) * pageNumber);
  uint32_t pagePos = 0;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t lutOffset = 0;
  uint32_t anchorMapOffset = 0;
  readSectionOffsets(f, lutOffset, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

// Paragraph-level KOSync (upstream PR #1686) is not supported in this fork.
// The section file format here does not carry paragraph indices in its LUT.
std::optional<uint16_t> Section::getPageForParagraphIndex(uint16_t /*pIndex*/) const { return std::nullopt; }
