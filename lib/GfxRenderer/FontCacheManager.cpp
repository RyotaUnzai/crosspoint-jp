#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

// Deduplicate SdCardFont pointers: multiple fontIds may point to the same SdCardFont
// (virtual fontIds for scaled sizes). With dual-base, at most 2 unique pointers exist.
// FNV-hashed fontIds may interleave in std::map iteration, so lastSeen is unreliable.
template <typename Fn>
static void forEachUniqueSdCardFont(const std::map<int, SdCardFont*>& sdCardFonts, Fn fn) {
  SdCardFont* seen[2] = {nullptr, nullptr};
  int seenCount = 0;
  for (auto& [id, font] : sdCardFonts) {
    bool already = false;
    for (int i = 0; i < seenCount; i++) {
      if (seen[i] == font) {
        already = true;
        break;
      }
    }
    if (!already) {
      fn(font);
      if (seenCount < 2) seen[seenCount++] = font;
    }
  }
}

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  forEachUniqueSdCardFont(sdCardFonts_, [](SdCardFont* f) { f->clearCache(); });
}

void FontCacheManager::logPrewarmMisses(const char* tag, const int missed, const uint8_t styleMask) {
  if (missed > 0) {
    LOG_DBG("FCM", "%s: %d glyph(s) not cached (styleMask=0x%02X)", tag, missed, styleMask);
  }
}

void FontCacheManager::freeKernLigatureData() {
  forEachUniqueSdCardFont(sdCardFonts_, [](SdCardFont* f) { f->freeKernLigatureData(); });
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    prewarmSdCardFont(it->second, utf8Text, styleMask);
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  prewarmCompressedFont(fontId, utf8Text, styleMask);
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  forEachUniqueSdCardFont(sdCardFonts_, [label](SdCardFont* f) { f->logStats(label); });
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  forEachUniqueSdCardFont(sdCardFonts_, [](SdCardFont* f) { f->resetStats(); });
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::resetPerFontScanState() {
  for (int i = 0; i < MAX_SCAN_FONTS; i++) {
    scanPerFont_[i].font = nullptr;
    scanPerFont_[i].text.clear();
    memset(scanPerFont_[i].styleCounts, 0, sizeof(scanPerFont_[i].styleCounts));
  }
  scanPerFontCount_ = 0;
}

void FontCacheManager::resetCompressedScanState() {
  scanCompressedText_.clear();
  scanCompressedText_.reserve(2048);
  memset(scanCompressedStyleCounts_, 0, sizeof(scanCompressedStyleCounts_));
  scanCompressedFontId_ = -1;
}

void FontCacheManager::resetScanState() {
  resetPerFontScanState();
  resetCompressedScanState();
}

uint8_t FontCacheManager::buildStyleMask(const uint32_t styleCounts[4]) {
  uint8_t styleMask = 0;
  for (uint8_t s = 0; s < 4; s++) {
    if (styleCounts[s] > 0) styleMask |= (1 << s);
  }
  return styleMask == 0 ? 1 : styleMask;
}

void FontCacheManager::recordScanTextForSdFont(SdCardFont* font, const char* text, const uint32_t cpCount,
                                               const uint8_t baseStyle) {
  int idx = -1;
  for (int i = 0; i < scanPerFontCount_; i++) {
    if (scanPerFont_[i].font == font) {
      idx = i;
      break;
    }
  }
  if (idx < 0 && scanPerFontCount_ < MAX_SCAN_FONTS) {
    idx = scanPerFontCount_++;
    scanPerFont_[idx].font = font;
    scanPerFont_[idx].text.reserve(512);
  }
  if (idx >= 0) {
    scanPerFont_[idx].text += text;
    scanPerFont_[idx].styleCounts[baseStyle] += cpCount;
  }
}

void FontCacheManager::recordScanTextForCompressedFont(const int fontId, const char* text, const uint32_t cpCount,
                                                       const uint8_t baseStyle) {
  scanCompressedText_ += text;
  if (scanCompressedFontId_ < 0) scanCompressedFontId_ = fontId;
  scanCompressedStyleCounts_[baseStyle] += cpCount;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }

  // Route text to the correct SdCardFont (per-font tracking for efficient prewarm)
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    recordScanTextForSdFont(it->second, text, cpCount, baseStyle);
  } else {
    recordScanTextForCompressedFont(fontId, text, cpCount, baseStyle);
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->resetScanState();
}

void FontCacheManager::prewarmRecordedScanData() {
  // Prewarm each SD card font with only its own text (per-font tracking)
  for (int i = 0; i < scanPerFontCount_; i++) {
    auto& entry = scanPerFont_[i];
    if (entry.text.empty() || !entry.font) continue;

    const uint8_t styleMask = buildStyleMask(entry.styleCounts);
    prewarmSdCardFont(entry.font, entry.text.c_str(), styleMask);
    entry.text.clear();
    entry.text.shrink_to_fit();
  }

  // Prewarm compressed (non-SD) font
  if (!scanCompressedText_.empty() && scanCompressedFontId_ >= 0) {
    const uint8_t styleMask = buildStyleMask(scanCompressedStyleCounts_);
    prewarmCompressedFont(scanCompressedFontId_, scanCompressedText_.c_str(), styleMask);
    scanCompressedText_.clear();
    scanCompressedText_.shrink_to_fit();
  }
}

void FontCacheManager::prewarmSdCardFont(SdCardFont* font, const char* utf8Text, uint8_t styleMask) {
  if (!font || !utf8Text || *utf8Text == '\0') return;
  int missed = font->prewarm(utf8Text, styleMask);
  logPrewarmMisses("prewarmCache(SD)", missed, styleMask);
}

void FontCacheManager::prewarmCompressedFont(int fontId, const char* utf8Text, uint8_t styleMask) {
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0 || !utf8Text || *utf8Text == '\0') return;

  const auto& font = fontMap_.at(fontId);
  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = font.getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    logPrewarmMisses("prewarmCache", missed, styleMask);
  }
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  manager_->prewarmRecordedScanData();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanMode_ is already None)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
