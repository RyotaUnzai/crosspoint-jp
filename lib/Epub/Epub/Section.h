#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

 void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool firstLineIndent, bool embeddedStyle, uint8_t imageRendering, bool verticalMode,
                              uint8_t charSpacing);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  CssParser* loadEmbeddedCssForSection(bool embeddedStyle);
  bool streamSpineItemToTempHtml(const std::string& localPath, const std::string& tmpHtmlPath,
                                 uint32_t& fileSize);
  bool readSectionOffsets(FsFile& file, uint32_t& lutOffset, uint32_t& anchorMapOffset) const;
  bool finalizeSectionFile(const std::vector<uint32_t>& lut,
                           const std::vector<std::pair<std::string, uint16_t>>& anchors,
                           const std::string& tmpSectionPath, CssParser* cssParser, uint32_t createSectionStart,
                           uint32_t parseBuildStart);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                       bool embeddedStyle, uint8_t imageRendering, bool verticalMode, uint8_t charSpacing);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                         bool embeddedStyle, uint8_t imageRendering, bool verticalMode, uint8_t charSpacing,
                         const std::function<void()>& popupFn = nullptr, const int* headingFontIds = nullptr,
                         int tableFontId = 0,
                         const std::function<void(uint16_t pagesDone, uint16_t estimatedPages)>& progressFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::unique_ptr<Page> loadPageFromSectionFile(uint16_t pageNumber);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
