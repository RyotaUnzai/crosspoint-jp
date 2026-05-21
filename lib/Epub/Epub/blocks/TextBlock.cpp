#include "TextBlock.h"

#include <algorithm>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

int TextBlock::rubyFontId = 0;

void TextBlock::collectCodepoints(std::vector<uint32_t>& out, size_t max) const {
  if (max == 0 || out.size() >= max) {
    return;
  }

  for (const auto& word : words) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&ptr))) {
      // Check if already exists (simple linear search, OK for small sets)
      bool exists = false;
      for (uint32_t existing : out) {
        if (existing == cp) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        out.push_back(cp);
        if (out.size() >= max) {
          return;
        }
      }
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const int viewportWidth) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }
  if (isVertical && !wordHeights.empty() && words.size() != wordHeights.size()) {
    LOG_ERR("TXB", "Render skipped: vertical size mismatch (words=%u, heights=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordHeights.size());
    return;
  }

  const int effectiveFontId = (blockStyle.fontId != 0) ? blockStyle.fontId : fontId;
  const bool rubyAvailable = (rubyFontId != 0) && hasRuby();

  int baseColumnWidth = 0;
  if (isVertical) {
    baseColumnWidth = renderer.getTextAdvanceX(effectiveFontId, "\xE4\xB8\x80", EpdFontFamily::REGULAR);
    if (baseColumnWidth <= 0) baseColumnWidth = renderer.getLineHeight(effectiveFontId);
    if (baseColumnWidth <= 0) baseColumnWidth = 1;
  }

  for (size_t i = 0; i < words.size(); i++) {
    const EpdFontFamily::Style currentStyle = wordStyles[i];

    if (isVertical && i < wordYpos.size()) {
      const char* w = words[i].c_str();
      const int wx = x + wordXpos[i];
      const int wy = y + wordYpos[i];

      const auto* p = reinterpret_cast<const unsigned char*>(w);
      uint32_t firstCp = utf8NextCodepoint(&p);
      bool isSingleCjk = (firstCp != 0 && *p == '\0' && VerticalTextUtils::isUprightInVertical(firstCp));

      if (isSingleCjk) {
        renderer.drawTextVertical(effectiveFontId, wx, wy, w, true, currentStyle);
        if (rubyAvailable && i < rubyTexts.size() && !rubyTexts[i].empty()) {
          const int rubyHeight = (i < rubySpans.size() && rubySpans[i] > 0)
                                     ? rubySpans[i]
                                     : renderer.measureTextVerticalSpan(rubyFontId, rubyTexts[i].c_str(),
                                                                         EpdFontFamily::REGULAR);
          const int rubyGap = std::max(4, renderer.getLineHeight(rubyFontId) / 4);
          const int rubyX = wx + baseColumnWidth + rubyGap;
          const int slotHeight = (!wordHeights.empty() && i < wordHeights.size()) ? wordHeights[i] : rubyHeight;
          const int rubyY = wy + std::max(0, (slotHeight - rubyHeight) / 2);
          renderer.drawTextVertical(rubyFontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
        }
      } else {
        bool allDigits = true;
        int asciiCount = 0;
        for (const char* c = w; *c; c++) {
          if ((static_cast<uint8_t>(*c) & 0xC0) != 0x80) asciiCount++;
          if (*c < '0' || *c > '9') allDigits = false;
        }
        if (allDigits && asciiCount <= 2) {
          const int textW = renderer.getTextAdvanceX(effectiveFontId, w, currentStyle);
          const int centerOffset = std::max(0, (baseColumnWidth - textW) / 2);
          renderer.drawText(effectiveFontId, wx + centerOffset, wy, w, true, currentStyle);
        } else {
          const int vertShift = renderer.getFontAscenderSize(effectiveFontId) / 3;
          renderer.drawTextSideways(effectiveFontId, wx, wy + vertShift, w, true, currentStyle, baseColumnWidth);
        }
      }
    } else {
      const int wordX = wordXpos[i] + x;
      const int baseWidth = renderer.getTextAdvanceX(effectiveFontId, words[i].c_str(), currentStyle);
      const int rubyWidth =
          (rubyAvailable && i < rubyTexts.size() && !rubyTexts[i].empty())
              ? ((i < rubyWidths.size() && rubyWidths[i] > 0)
                     ? rubyWidths[i]
                     : renderer.getTextWidth(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR))
              : 0;
      const int slotWidth = std::max(1, std::max(baseWidth, rubyWidth));
      const int baseX = wordX + (slotWidth - baseWidth) / 2;
      renderer.drawText(effectiveFontId, baseX, y, words[i].c_str(), true, currentStyle);

      if (rubyAvailable && i < rubyTexts.size() && !rubyTexts[i].empty()) {
        const int rubyX = wordX + (slotWidth - rubyWidth) / 2;
        const int rubyY = y - renderer.getLineHeight(rubyFontId) - 1;
        renderer.drawText(rubyFontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
      }

      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const std::string& w = words[i];
        const int fullWordWidth = renderer.getTextWidth(effectiveFontId, w.c_str(), currentStyle);
        const int underlineY = y + renderer.getFontAscenderSize(effectiveFontId) + 2;

        int startX = wordX;
        int underlineWidth = fullWordWidth;

        if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
            static_cast<uint8_t>(w[2]) == 0x83) {
          const char* visiblePtr = w.c_str() + 3;
          const int prefixWidth = renderer.getTextAdvanceX(effectiveFontId, "\xE2\x80\x83", currentStyle);
          const int visibleWidth = renderer.getTextWidth(effectiveFontId, visiblePtr, currentStyle);
          startX = wordX + prefixWidth;
          underlineWidth = visibleWidth;
        }

        renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
      }
    }
  }

  if (blockStyle.drawSeparatorBelow && viewportWidth > 0 && !isVertical) {
    const int separatorY = y + renderer.getLineHeight(effectiveFontId) + 2;
    renderer.drawLine(0, separatorY, viewportWidth, separatorY, true);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", words.size(),
            wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.fontId);
  serialization::writePod(file, blockStyle.drawSeparatorBelow);
  serialization::writePod(file, blockStyle.isListItem);
  serialization::writePod(file, static_cast<uint16_t>(rubyWidths.size()));
  for (auto w : rubyWidths) serialization::writePod(file, w);
  serialization::writePod(file, static_cast<uint16_t>(rubySpans.size()));
  for (auto s : rubySpans) serialization::writePod(file, s);
  serialization::writePod(file, verticalRubyWidth);

  // Vertical layout data
  serialization::writePod(file, isVertical);
  if (isVertical) {
    for (auto y : wordYpos) serialization::writePod(file, y);
    for (auto h : wordHeights) serialization::writePod(file, h);
  }

  // Ruby text data
  for (size_t i = 0; i < words.size(); i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.fontId);
  serialization::readPod(file, blockStyle.drawSeparatorBelow);
  serialization::readPod(file, blockStyle.isListItem);
  uint16_t rubyWidthCount = 0;
  serialization::readPod(file, rubyWidthCount);
  std::vector<int16_t> rubyWidths;
  rubyWidths.resize(rubyWidthCount);
  for (auto& w : rubyWidths) serialization::readPod(file, w);
  uint16_t rubySpanCount = 0;
  serialization::readPod(file, rubySpanCount);
  std::vector<int16_t> rubySpans;
  rubySpans.resize(rubySpanCount);
  for (auto& s : rubySpans) serialization::readPod(file, s);
  int16_t verticalRubyWidth = 0;
  serialization::readPod(file, verticalRubyWidth);

  // Vertical layout data
  bool vertical = false;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  std::vector<int16_t> wordHeights;
  if (vertical) {
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
    wordHeights.resize(wc);
    for (auto& h : wordHeights) serialization::readPod(file, h);
  }

  // Ruby text data
  std::vector<std::string> rubyTexts(wc);
  for (auto& rt : rubyTexts) serialization::readString(file, rt);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles),
                                                  blockStyle, std::move(wordYpos), std::move(wordHeights), vertical,
                                                  std::move(rubyTexts), std::move(rubyWidths), std::move(rubySpans),
                                                  verticalRubyWidth));
}
