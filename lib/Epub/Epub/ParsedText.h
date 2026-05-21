#pragma once

#include <EpdFontFamily.h>
#include <VerticalTextUtils.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;  // true = word attaches to previous (no space before it)
  std::vector<std::string> rubyTexts;
  std::vector<size_t> rubyWordIndices;
  std::vector<uint16_t> rubyWidths;
  std::vector<uint16_t> rubySpans;
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
  size_t rubyWordCount = 0;
  size_t sdFontReadyWordCount = 0;
  uint8_t cachedStyleMask = 0;
  BlockStyle blockStyle;
  bool firstLineIndent;
  bool hyphenationEnabled;

  void applyParagraphIndent();
  uint8_t getStyleMask() const;
  void prepareRubyMetrics(const GfxRenderer& renderer, int fontId, bool needVerticalSpan);
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& wordIsCjkVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec, std::vector<bool>& wordIsCjkVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            std::vector<bool>* continuesVec = nullptr, std::vector<bool>* wordIsCjkVec = nullptr);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& wordIsCjkVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const BlockStyle& blockStyle = BlockStyle(),
                      const bool firstLineIndent = false)
      : blockStyle(blockStyle), firstLineIndent(firstLineIndent), hyphenationEnabled(hyphenationEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void addWord(std::string word, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vBehavior,
               bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  void setRubyForWordAt(size_t index, const std::string& ruby);
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }
  size_t getRubyWordCount() const { return rubyWordCount; }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                             bool includeLastColumn = true);
};
