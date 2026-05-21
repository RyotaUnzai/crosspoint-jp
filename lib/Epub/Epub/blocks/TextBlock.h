#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 private:
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;
  std::vector<int16_t> wordYpos;  // vertical layout: y position within column
  std::vector<int16_t> wordHeights;  // vertical layout: reserved height for each word slot
  bool isVertical = false;        // true when this block was laid out vertically
  std::vector<std::string> rubyTexts;
  std::vector<int16_t> rubyWidths;
  std::vector<int16_t> rubySpans;
  int16_t verticalRubyWidth = 0;

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<int16_t> word_ypos = {}, std::vector<int16_t> word_heights = {}, bool vertical = false,
                     std::vector<std::string> ruby_texts = {}, std::vector<int16_t> ruby_widths = {},
                     std::vector<int16_t> ruby_spans = {}, int16_t vertical_ruby_width = 0)
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle),
        wordYpos(std::move(word_ypos)),
        wordHeights(std::move(word_heights)),
        isVertical(vertical),
        rubyTexts(std::move(ruby_texts)),
        rubyWidths(std::move(ruby_widths)),
        rubySpans(std::move(ruby_spans)),
        verticalRubyWidth(vertical_ruby_width) {}
  ~TextBlock() override = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  const std::vector<int16_t>& getWordYpos() const { return wordYpos; }
  bool getIsVertical() const { return isVertical; }
  bool hasRuby() const;
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }
  int16_t getVerticalRubyWidth() const { return verticalRubyWidth; }
  static int rubyFontId;
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y, int viewportWidth = 0) const;
  void collectCodepoints(std::vector<uint32_t>& out, size_t max) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
