#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <esp_system.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/LineSpacingSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};
constexpr uint32_t HEAVY_SECTION_BUILD_THRESHOLD_MS = 12000;

double msToSeconds(const uint32_t elapsedMs) {
  return static_cast<double>(elapsedMs) / 1000.0;
}

std::string heavyModeFlagPath(const std::shared_ptr<Epub>& epub) {
  return epub->getCachePath() + "/.heavy_mode";
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

struct BackgroundCacheJob {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<GfxRenderer> renderer;
  std::unique_ptr<FontCacheManager> fontCacheManager;
  std::vector<int> spineOrder;
  int readerFontId = 0;
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
  int headingFontIds[6] = {};
  int tableFontId = 0;
  bool includeImages = false;
  std::shared_ptr<std::atomic_bool> cancelFlag;
  std::shared_ptr<std::atomic_bool> runningFlag;
  std::shared_ptr<std::atomic_int> progressPercent;
};

std::unique_ptr<GfxRenderer> cloneRendererForBackground(const GfxRenderer& source) {
  auto clone = std::make_unique<GfxRenderer>(display);
  clone->setOrientation(source.getOrientation());
  clone->setDarkMode(source.isDarkMode());
  clone->setInvertImagesInDarkMode(source.shouldInvertImagesInDarkMode());
  clone->setAsciiLetterSpacing(source.getAsciiLetterSpacing());
  clone->setAsciiDigitSpacing(source.getAsciiDigitSpacing());
  clone->setCjkSpacing(source.getCjkSpacing());
  clone->setReaderFallbackFontId(source.getReaderFallbackFontId());
  clone->setVerticalCharSpacing(source.getVerticalCharSpacing());

  for (const auto& [fontId, fontFamily] : source.getFontMap()) {
    clone->insertFont(fontId, fontFamily);
  }
  for (const auto& [fontId, sdFont] : source.getSdCardFonts()) {
    clone->registerSdCardFont(fontId, sdFont);
    clone->registerSdCardFontScale(fontId, source.getSdCardFontScale(fontId));
  }

  return clone;
}

std::unique_ptr<FontCacheManager> cloneFontCacheManager(const GfxRenderer& source, GfxRenderer& clone) {
  auto cacheManager = std::make_unique<FontCacheManager>(clone.getFontMap(), clone.getSdCardFonts());
  if (auto* sourceCache = source.getFontCacheManager()) {
    cacheManager->setFontDecompressor(sourceCache->getDecompressor());
  }
  clone.setFontCacheManager(cacheManager.get());
  return cacheManager;
}

void buildSectionImageCache(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t viewportWidth,
                            uint16_t viewportHeight) {
  const std::string imgPrefix = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";
  for (int j = 0;; j++) {
    std::string jpgPath = imgPrefix + std::to_string(j) + ".jpg";
    if (!Storage.exists(jpgPath.c_str())) {
      jpgPath = imgPrefix + std::to_string(j) + ".jpeg";
      if (!Storage.exists(jpgPath.c_str())) break;
    }

    const size_t dotPos = jpgPath.rfind('.');
    const std::string bmpCachePath = jpgPath.substr(0, dotPos) + ".pxc.bmp";
    if (Storage.exists(bmpCachePath.c_str())) continue;

    FsFile jpegFile, bmpFile;
    if (Storage.openFileForRead("PRE", jpgPath, jpegFile) && Storage.openFileForWrite("PRE", bmpCachePath, bmpFile)) {
      JpegToBmpConverter::jpegFileToBmpStreamWithSize(jpegFile, bmpFile, viewportWidth, viewportHeight);
      jpegFile.close();
      bmpFile.close();
    }
  }
}

void appendSpineRange(std::vector<int>& spineOrder, const int startSpineIndex, const int endSpineIndex,
                      const int spineCount) {
  if (endSpineIndex < 0 || endSpineIndex > spineCount) {
    spineOrder.reserve(static_cast<size_t>(spineCount - startSpineIndex));
    for (int i = startSpineIndex; i < spineCount; i++) {
      spineOrder.push_back(i);
    }
    return;
  }

  spineOrder.reserve(static_cast<size_t>(std::max(0, endSpineIndex - startSpineIndex)));
  for (int i = startSpineIndex; i < endSpineIndex; i++) {
    spineOrder.push_back(i);
  }
}

void runBackgroundCacheJob(std::unique_ptr<BackgroundCacheJob> job) {
  job->runningFlag->store(true);

  const int spineCountLocal = job->epub->getSpineItemsCount();
  const int totalSections = std::max(1, static_cast<int>(job->spineOrder.size()));
  for (size_t n = 0; n < job->spineOrder.size(); n++) {
    if (job->cancelFlag->load()) break;
    const int spineIndex = job->spineOrder[n];
    if (spineIndex < 0 || spineIndex >= spineCountLocal) continue;

    Section sec(job->epub, spineIndex, *job->renderer);
    if (sec.loadSectionFile(job->readerFontId, job->lineCompression, job->extraParagraphSpacing,
                            job->paragraphAlignment, job->viewportWidth, job->viewportHeight,
                            job->hyphenationEnabled, job->firstLineIndent, job->embeddedStyle,
                            job->imageRendering, job->verticalMode, job->charSpacing)) {
      job->progressPercent->store(clampPercent(static_cast<int>((100.0f * (n + 1)) / totalSections + 0.5f)));
      continue;
    }

    if (job->cancelFlag->load()) break;

    const auto progressPercent = job->progressPercent;
    const auto progressFn = [progressPercent, n, totalSections](uint16_t pagesDone, uint16_t estimatedPages) {
      const float sectionProgress = (estimatedPages > 0) ? (static_cast<float>(pagesDone) / estimatedPages) : 0.0f;
      const float overallProgress = (static_cast<float>(n) + sectionProgress) / static_cast<float>(totalSections);
      progressPercent->store(clampPercent(static_cast<int>(overallProgress * 100.0f + 0.5f)));
    };

    if (!sec.createSectionFile(job->readerFontId, job->lineCompression, job->extraParagraphSpacing,
                               job->paragraphAlignment, job->viewportWidth, job->viewportHeight,
                               job->hyphenationEnabled, job->firstLineIndent, job->embeddedStyle,
                               job->imageRendering, job->verticalMode, job->charSpacing, nullptr,
                               job->headingFontIds, job->tableFontId, progressFn)) {
      LOG_ERR("ERS", "Background indexing failed for section %d", spineIndex);
      continue;
    }

    if (job->includeImages && !job->cancelFlag->load()) {
      buildSectionImageCache(job->epub, spineIndex, job->viewportWidth, job->viewportHeight);
    }

    job->progressPercent->store(clampPercent(static_cast<int>((100.0f * (n + 1)) / totalSections + 0.5f)));
    activityManager.requestUpdate(true);
  }

  job->progressPercent->store(100);
  job->runningFlag->store(false);
}

}  // namespace

void EpubReaderActivity::loadHeavyBookMode() {
  if (!epub) return;

  heavyBookModeKnown = true;
  heavyBookMode = Storage.exists(heavyModeFlagPath(epub).c_str());
  if (heavyBookMode) {
    LOG_DBG("ERS", "Heavy book mode enabled for this title");
  }
}

void EpubReaderActivity::markHeavyBookMode() {
  if (!epub) return;
  if (heavyBookMode) return;

  heavyBookMode = true;
  heavyBookModeKnown = true;

  const std::string flagPath = heavyModeFlagPath(epub);
  FsFile f;
  if (Storage.openFileForWrite("ERS", flagPath, f)) {
    f.close();
    LOG_DBG("ERS", "Marked heavy book mode: %s", flagPath.c_str());
  } else {
    LOG_ERR("ERS", "Failed to persist heavy book flag: %s", flagPath.c_str());
  }
}

void EpubReaderActivity::cancelBackgroundCacheGeneration() {
  if (backgroundCacheCancelFlag) {
    backgroundCacheCancelFlag->store(true);
  }
}

void EpubReaderActivity::clearPrefetchedNextPage() {
  prefetchedNextPage.reset();
  prefetchedNextPageSpineIndex = -1;
  prefetchedNextPageNumber = -1;
}

void EpubReaderActivity::resetTransientReaderState(const bool cancelBackgroundCache) {
  if (cancelBackgroundCache) {
    cancelBackgroundCacheGeneration();
  }
  clearPrefetchedNextPage();
}

std::unique_ptr<Page> EpubReaderActivity::loadPageForRender() {
  if (!section) {
    return nullptr;
  }

  if (prefetchedNextPage && prefetchedNextPageSpineIndex == currentSpineIndex &&
      prefetchedNextPageNumber == section->currentPage) {
    auto page = std::move(prefetchedNextPage);
    clearPrefetchedNextPage();
    LOG_DBG("ERS", "Using prefetched page %d for spine %d", section->currentPage, currentSpineIndex);
    return page;
  }

  return section->loadPageFromSectionFile();
}

bool EpubReaderActivity::shouldPrefetchNextPage() const {
  if (!epub || !section) {
    return false;
  }

  // Only prefetch the first forward page of a chapter. This keeps the change targeted:
  // the opener page often contains only title/blank space, and the next page is the one
  // users want immediately.
  return section->currentPage == 0 && section->pageCount >= 2;
}

void EpubReaderActivity::prefetchNextPageIfHelpful(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!shouldPrefetchNextPage()) {
    return;
  }

  const int nextPage = 1;
  if (prefetchedNextPage && prefetchedNextPageSpineIndex == currentSpineIndex &&
      prefetchedNextPageNumber == nextPage) {
    return;
  }

  const int savedPage = section->currentPage;
  section->currentPage = nextPage;
  auto page = section->loadPageFromSectionFile();
  section->currentPage = savedPage;
  if (!page) {
    clearPrefetchedNextPage();
    return;
  }

  prefetchedNextPage = std::move(page);
  prefetchedNextPageSpineIndex = currentSpineIndex;
  prefetchedNextPageNumber = nextPage;
  LOG_DBG("ERS", "Prefetched next page %d for spine %d (%ux%u)", nextPage, currentSpineIndex, viewportWidth,
          viewportHeight);
}

void EpubReaderActivity::startBackgroundCacheGeneration(const int startSpineIndex, const int endSpineIndex,
                                                        const bool includeImages, const uint16_t viewportWidth,
                                                        const uint16_t viewportHeight) {
  if (!epub) {
    return;
  }
  if (!backgroundCacheRunningFlag->load()) {
    backgroundCacheTaskHandle = nullptr;
  } else {
    return;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0 || startSpineIndex < 0 || startSpineIndex >= spineCount) {
    return;
  }

  auto job = std::make_unique<BackgroundCacheJob>();
  job->epub = epub;
  job->renderer = cloneRendererForBackground(renderer);
  job->fontCacheManager = cloneFontCacheManager(renderer, *job->renderer);

  job->readerFontId = SETTINGS.getReaderFontId(verticalMode);
  job->lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);
  job->extraParagraphSpacing = ds.extraParagraphSpacing;
  job->paragraphAlignment = ds.paragraphAlignment;
  job->viewportWidth = viewportWidth;
  job->viewportHeight = viewportHeight;
  job->hyphenationEnabled = ds.hyphenationEnabled;
  job->firstLineIndent = ds.firstLineIndent;
  job->embeddedStyle = SETTINGS.embeddedStyle;
  job->imageRendering = SETTINGS.imageRendering;
  job->verticalMode = verticalMode;
  job->charSpacing = ds.charSpacing;
  job->headingFontIds[0] = SETTINGS.getHeadingFontId(1, verticalMode);
  job->headingFontIds[1] = SETTINGS.getHeadingFontId(2, verticalMode);
  job->tableFontId = SETTINGS.getTableFontId(verticalMode);
  job->includeImages = includeImages;
  job->cancelFlag = backgroundCacheCancelFlag;
  job->runningFlag = backgroundCacheRunningFlag;
  job->progressPercent = backgroundCacheProgressPercent;
  backgroundCacheProgressPercent->store(0);
  appendSpineRange(job->spineOrder, startSpineIndex, endSpineIndex, spineCount);

  if (!includeImages) {
    job->spineOrder.shrink_to_fit();
  }

  backgroundCacheCancelFlag->store(false);
  backgroundCacheRunningFlag->store(true);

  auto* rawJob = job.release();
  const BaseType_t rc = xTaskCreate(backgroundCacheTaskTrampoline, "EpubBgCache", 8192, rawJob, 0,
                                    &backgroundCacheTaskHandle);

  if (rc != pdPASS || backgroundCacheTaskHandle == nullptr) {
    backgroundCacheRunningFlag->store(false);
    delete rawJob;
    backgroundCacheTaskHandle = nullptr;
    LOG_ERR("ERS", "Failed to start background cache task");
  } else {
    LOG_DBG("ERS", "Started background cache task for sections %d..%d", startSpineIndex, endSpineIndex);
  }
}

void EpubReaderActivity::backgroundCacheTaskTrampoline(void* param) {
  auto job = std::unique_ptr<BackgroundCacheJob>(static_cast<BackgroundCacheJob*>(param));
  runBackgroundCacheJob(std::move(job));
  vTaskDelete(nullptr);
}

void EpubReaderActivity::pregenerateCache() {
  if (!epub) return;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;

  // Resolve writing mode first (needed for direction-specific settings)
  bool isVertical = false;
  if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
    isVertical = true;
  } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
    isVertical = false;
  } else {
    isVertical = epub && epub->isPageProgressionRtl() &&
                 (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" || epub->getLanguage() == "zh" ||
                  epub->getLanguage() == "zho");
  }

  const auto& ds = SETTINGS.getDirectionSettings(isVertical);
  ensureSdFontLoaded(isVertical);

  // Calculate viewport dimensions (same logic as render())
  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += ds.screenMargin;
  orientedMarginRight += ds.screenMargin;
  orientedMarginLeft += ds.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  orientedMarginBottom += std::max(ds.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  // Free font cache to maximize heap for section building
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  const int headingFontIds[6] = {
      SETTINGS.getHeadingFontId(1, isVertical), SETTINGS.getHeadingFontId(2, isVertical), 0, 0, 0, 0};

  int completedSections = 0;
  int lastProgressPercent = -1;
  uint32_t lastProgressUpdateMs = 0;
  Rect popupRect{};

  auto renderProgress = [&](int percent, bool force = false) {
    percent = clampPercent(percent);
    const uint32_t now = millis();
    if (!force) {
      if (percent == lastProgressPercent) return;
      if (lastProgressPercent >= 0 && percent < 100 && (now - lastProgressUpdateMs) < 1000) return;
      if (lastProgressPercent >= 0 && percent > lastProgressPercent && (percent - lastProgressPercent) < 5 &&
          (now - lastProgressUpdateMs) < 1000) {
        return;
      }
    }

    if (percent == lastProgressPercent && !force) return;
    lastProgressPercent = percent;
    lastProgressUpdateMs = now;

    const std::string progressText = std::string("Progress ") + std::to_string(percent) + "%";
    popupRect = GUI.drawPopup(renderer, tr(STR_GENERATING_CACHE), progressText.c_str());
    GUI.fillPopupProgress(renderer, popupRect, percent);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  popupRect = GUI.drawPopup(renderer, tr(STR_GENERATING_CACHE), "Progress 0%");
  GUI.fillPopupProgress(renderer, popupRect, 0);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  lastProgressPercent = 0;

  const int pregenerateStart = std::clamp(currentSpineIndex, 0, spineCount - 1);
  const int pregenerateEnd = pregenerateStart + 1;

  for (int i = pregenerateStart; i < pregenerateEnd; i++) {
    {
      const int adc1 = analogRead(1);
      const int adc2 = analogRead(2);
      constexpr int ADC_NO_BUTTON = 3800;
      if (adc1 < ADC_NO_BUTTON || adc2 < ADC_NO_BUTTON) {
        LOG_DBG("ERS", "Pregenerate cancelled at section %d/%d", i, spineCount);
        break;
      }
    }

    const auto progressFn = [this, &renderProgress, completedSections, spineCount](uint16_t pagesDone,
                                                                                   uint16_t estimatedPages) mutable {
      if (spineCount <= 0) {
        renderProgress(100);
        return;
      }
      const float sectionProgress = (estimatedPages > 0) ? (static_cast<float>(pagesDone) / estimatedPages) : 0.0f;
      const float overallProgress = (static_cast<float>(completedSections) + sectionProgress) /
                                    static_cast<float>(std::max(1, spineCount));
      const int smoothProgress = clampPercent(static_cast<int>(overallProgress * 100.0f + 0.5f));
      renderProgress(smoothProgress >= 100 ? 99 : smoothProgress);
    };

    Section sec(epub, i, renderer);
    if (sec.loadSectionFile(SETTINGS.getReaderFontId(isVertical), SETTINGS.getReaderLineCompression(isVertical),
                            ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth, viewportHeight,
                            ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                            SETTINGS.imageRendering, isVertical, ds.charSpacing)) {
      completedSections++;
      continue;
    }

    if (!sec.createSectionFile(SETTINGS.getReaderFontId(isVertical), SETTINGS.getReaderLineCompression(isVertical),
                               ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth, viewportHeight,
                               ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                               SETTINGS.imageRendering, isVertical, ds.charSpacing, nullptr, headingFontIds,
                               SETTINGS.getTableFontId(isVertical), progressFn)) {
      LOG_ERR("ERS", "Pregenerate: failed section %d (heap: %d)", i, ESP.getFreeHeap());
      continue;
    }
    completedSections++;
  }

  renderProgress(100, true);
  delay(150);
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // ルビフォントIDはrender()内でフォントロード後に設定

  // Screen orientation (both renderer and input) is already set by
  // enterNewActivity() → OrientationHelper::applyOrientation() before onEnter().

  epub->setupCacheDir();
  loadHeavyBookMode();
  pagesUntilFullRefresh = ReaderUtils::getRefreshCyclePages();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Check if section cache exists; offer to pregenerate if missing
  const std::string firstSectionPath = epub->getCachePath() + "/sections/0.bin";
  const std::string noCachePromptPath = epub->getCachePath() + "/.no_cache_prompt";
  if (!Storage.exists(firstSectionPath.c_str()) && !Storage.exists(noCachePromptPath.c_str())) {
    auto handler = [this, noCachePromptPath](const ActivityResult& res) {
      if (!res.isCancelled) {
        // "生成" — generate cache and continue
        pregenerateCache();
        requestUpdate();
      } else if (auto* menu = std::get_if<MenuResult>(&res.data)) {
        if (menu->action == ConfirmationActivity::RESULT_NEVER) {
          // "しない" — write flag file so we never ask again, then continue
          FsFile f;
          if (Storage.openFileForWrite("ERS", noCachePromptPath, f)) {
            f.close();
          }
          requestUpdate();
        }
      } else {
        // "閉じる" — close the book and go home
        onGoHome();
      }
    };
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_GENERATE_CACHE),
                                               tr(STR_GENERATE_CACHE_NOTE), tr(STR_SKIP_CACHE), tr(STR_GENERATE)),
        handler);
    return;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  cancelBackgroundCacheGeneration();
  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  refreshBackgroundCacheTaskHandle();

  if (handleAutomaticPageTurn()) {
    return;
  }

  if (handleReaderMenuOpen()) {
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  handlePageTurnInput();
}

void EpubReaderActivity::refreshBackgroundCacheTaskHandle() {
  if (backgroundCacheTaskHandle != nullptr && !backgroundCacheRunningFlag->load()) {
    backgroundCacheTaskHandle = nullptr;
  }
}

bool EpubReaderActivity::handleAutomaticPageTurn() {
  if (!automaticPageTurnActive) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    automaticPageTurnActive = false;
    // updates chapter title space to indicate page turn disabled
    requestUpdate();
    return true;
  }

  if (!section) {
    requestUpdate();
    return true;
  }

  // Skips page turn if renderingMutex is busy
  if (RenderLock::peek()) {
    lastPageTurnTime = millis();
    return true;
  }

  if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
    pageTurn(true);
    return true;
  }

  return false;
}

bool EpubReaderActivity::handleReaderMenuOpen() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    return false;
  }

  // Snapshot reader state under render lock. This avoids racing with the
  // render task while it may rebuild/reset the current section.
  int menuSpineIndex = 0;
  int menuCurrentPage = 0;
  int menuTotalPages = 0;
  {
    RenderLock lock(*this);
    menuSpineIndex = currentSpineIndex;
    if (section) {
      menuCurrentPage = section->currentPage + 1;
      menuTotalPages = section->pageCount;
    }
  }

  if (!epub) return true;

  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && menuTotalPages > 0) {
    const float chapterProgress = static_cast<float>(menuCurrentPage - 1) / static_cast<float>(menuTotalPages);
    bookProgress = epub->calculateProgress(menuSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(),
                                                                  menuCurrentPage, menuTotalPages, bookProgressPercent,
                                                                  SETTINGS.orientation, !currentPageFootnotes.empty(),
                                                                  verticalMode),
                         [this](const ActivityResult& result) {
                           // Always apply orientation change even if the menu was cancelled
                           const auto& menu = std::get<MenuResult>(result.data);
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
  return true;
}

bool EpubReaderActivity::handleBackNavigation() {
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return true;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return true;
    }
    onGoHome();
    return true;
  }

  return false;
}

bool EpubReaderActivity::handlePageTurnInput() {
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return false;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
    return true;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    lastPageTurnTime = millis();
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      const bool skipForward = verticalMode ? !nextTriggered : nextTriggered;
      currentSpineIndex = skipForward ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return true;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return true;
  }

  if (prevTriggered) {
    pageTurn(verticalMode);  // In vertical RTL: prev button = forward
  } else {
    pageTurn(!verticalMode);  // In vertical RTL: next button = backward
  }
  return true;
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    resetTransientReaderState(true);
    section.reset();
  }
}

void EpubReaderActivity::invalidateSectionPreservingPosition() {
  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  resetTransientReaderState(false);
  section.reset();
}

void EpubReaderActivity::openChapterSelection() {
  const int spineIdx = currentSpineIndex;
  const std::string path = epub->getPath();
  startActivityForResult(
      std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
          RenderLock lock(*this);
          currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
          nextPageNumber = 0;
          resetTransientReaderState(true);
          section.reset();
        }
      });
}

void EpubReaderActivity::openFootnotes() {
  startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                             navigateToHref(footnoteResult.href, true);
                           }
                           requestUpdate();
                         });
}

void EpubReaderActivity::openPercentSelection() {
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             jumpToPercent(std::get<PercentResult>(result.data).percent);
                           }
                         });
}

void EpubReaderActivity::openReaderSettings() {
  // Open settings directly on the Reader category and return to reader on back.
  startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, [this] { finish(); }, 1, 1),
                         [this](const ActivityResult&) {
                           // Reader settings (font/line spacing/margins etc.) may change pagination.
                           // Cache dir may have been deleted by ClearCacheActivity — recreate it.
                           if (epub) epub->setupCacheDir();
                           invalidateSectionPreservingPosition();
                           requestUpdate();
                         });
}

void EpubReaderActivity::displayPageQr() {
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    auto p = section->loadPageFromSectionFile();
    if (p) {
      std::string fullText;
      for (const auto& el : p->elements) {
        if (el->getTag() == TAG_PageLine) {
          const auto& line = static_cast<const PageLine&>(*el);
          if (line.getBlock()) {
            const auto& words = line.getBlock()->getWords();
            for (const auto& w : words) {
              if (!fullText.empty()) fullText += " ";
              fullText += w;
            }
          }
        }
      }
      if (!fullText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                               [this](const ActivityResult& result) {});
        return;
      }
    }
  }

  // If no text or page loading failed, just close menu
  requestUpdate();
}

void EpubReaderActivity::clearCacheAndGoHome() {
  {
    RenderLock lock(*this);
    if (epub && section) {
      uint16_t backupSpine = currentSpineIndex;
      uint16_t backupPage = section->currentPage;
      uint16_t backupPageCount = section->pageCount;
      section.reset();
      epub->clearCache();
      epub->setupCacheDir();
      saveProgress(backupSpine, backupPage, backupPageCount);
    }
  }
  onGoHome();
}

void EpubReaderActivity::syncReaderState() {
  if (!KOREADER_STORE.hasCredentials()) {
    return;
  }

  const int currentPage = section ? section->currentPage : 0;
  const int totalPages = section ? section->pageCount : 0;
  startActivityForResult(
      std::make_unique<KOReaderSyncActivity>(renderer, mappedInput, epub, epub->getPath(), currentSpineIndex,
                                             currentPage, totalPages),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          return;
        }

        const auto& sync = std::get<SyncResult>(result.data);
        if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
          RenderLock lock(*this);
          currentSpineIndex = sync.spineIndex;
          nextPageNumber = sync.page;
          resetTransientReaderState(true);
          section.reset();
        }
      });
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER:
      openChapterSelection();
      break;
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES:
      openFootnotes();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT:
      openPercentSelection();
      break;
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS:
      openReaderSettings();
      break;
    case EpubReaderMenuActivity::MenuAction::STYLE_FIRST_LINE_INDENT: {
      auto& dirSettings = SETTINGS.getDirectionSettings(verticalMode);
      dirSettings.firstLineIndent = !dirSettings.firstLineIndent;
      SETTINGS.saveToFile();
      invalidateSectionPreservingPosition();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_INVERT_IMAGES: {
      SETTINGS.invertImages = !SETTINGS.invertImages;
      SETTINGS.saveToFile();
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_LINE_SPACING: {
      uint8_t& target = SETTINGS.getDirectionSettings(verticalMode).lineSpacing;
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) {
                               invalidateSectionPreservingPosition();
                              requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_STATUS_BAR:
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR:
      displayPageQr();
      break;
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      return;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE:
      clearCacheAndGoHome();
      return;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::SYNC:
      syncReaderState();
      break;
    case EpubReaderMenuActivity::MenuAction::TILT_PAGE_TURN:
      // Toggled inline in menu; IMU sync handled in result callback.
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer and input orientation to match the new coordinate system.
    OrientationHelper::applyOrientation(renderer, mappedInput, this);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    resetTransientReaderState(false);
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        resetTransientReaderState(true);
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        resetTransientReaderState(true);
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    saveProgress(currentSpineIndex, 0, 0, true);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  // Resolve effective writing mode before viewport calculation (needed for direction-specific settings)
  if (!section) {
    if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
      verticalMode = true;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
      verticalMode = false;
    } else {
      // Auto: check OPF hints
      verticalMode = epub && epub->isPageProgressionRtl() &&
                     (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" || epub->getLanguage() == "zh" ||
                      epub->getLanguage() == "zho");
    }
  }

  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += ds.screenMargin;
  orientedMarginLeft += ds.screenMargin;
  orientedMarginRight += ds.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(ds.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(ds.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    // Ensure the correct SD card font is loaded for the resolved writing direction.
    // goToReader() calls ensureSdFontLoaded(false) before verticalMode is known,
    // so we reload here with the correct direction after resolution.
    ensureSdFontLoaded(verticalMode);

    // ルビ用フォント: フォントロード後に8ptフォントを取得
    // rubyEnabled が OFF の場合は rubyFontId=0 でルビ描画をスキップ
    {
      const auto& rubyDs = SETTINGS.getDirectionSettings(verticalMode);
      if (!rubyDs.rubyEnabled) {
        TextBlock::rubyFontId = 0;
      } else {
        static constexpr uint8_t RUBY_FONT_SIZE_ENUM = 5;  // 8pt
        int rubyId = 0;
        if (rubyDs.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
          rubyId = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, rubyDs.sdFontFamilyName, RUBY_FONT_SIZE_ENUM);
        }
        if (rubyId == 0) rubyId = SETTINGS.getReaderFontId(verticalMode);
        TextBlock::rubyFontId = rubyId;
      }
    }

    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);

    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u, vertical=%d", ds.lineSpacing,
            lineCompression, viewportWidth, viewportHeight, verticalMode);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing,
                                  ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                  ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode,
                                  ds.charSpacing)) {
      LOG_DBG("ERS", "Cache not found, building...");

        // Apply vertical character spacing for layout calculation
        renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

        // Free SD card font data before section building to maximize available heap.
        // clearCache() frees prewarm data (~130KB: miniGlyphs + miniBitmap).
        // freeKernLigatureData() frees kern/ligature tables (~22KB per style).
        // Both are lazy-loaded again during the render pass.
        auto* fcm = renderer.getFontCacheManager();
        if (fcm) {
          fcm->clearCache();
          fcm->freeKernLigatureData();
        }

        const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

        const int headingFontIds[6] = {
            SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};

        const uint32_t sectionBuildStart = millis();
        if (!section->createSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression,
                                        ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth,
                                        viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, verticalMode, ds.charSpacing, popupFn, headingFontIds,
                                        SETTINGS.getTableFontId(verticalMode))) {
          LOG_ERR("ERS", "Failed to persist page data to SD (free heap: %d)", ESP.getFreeHeap());
          section.reset();
          // Show error and return to home to avoid infinite retry loop
          // (loop() would call requestUpdate() → render() → same failure)
          renderer.clearScreen();
          renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          vTaskDelay(pdMS_TO_TICKS(300));
          onGoHome();
          return;
        }
        const uint32_t sectionBuildElapsed = millis() - sectionBuildStart;
        LOG_DBG("ERS", "Section build took %lu ms (%.2f s)", sectionBuildElapsed,
                msToSeconds(sectionBuildElapsed));
        if (sectionBuildElapsed >= HEAVY_SECTION_BUILD_THRESHOLD_MS) {
          markHeavyBookMode();
        }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    std::unique_ptr<Page> p = loadPageForRender();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      resetTransientReaderState(false);
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    const uint32_t renderElapsedMs = millis() - start;
    LOG_DBG("ERS", "Rendered page in %lu ms (%.2f s)", renderElapsedMs, msToSeconds(renderElapsedMs));
  }
  prefetchNextPageIfHelpful(viewportWidth, viewportHeight);
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  {
    bool nearEnd = false;
    if (epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress =
          static_cast<float>(section->currentPage + 1) / static_cast<float>(section->pageCount);
      nearEnd = epub->calculateProgress(currentSpineIndex, chapterProgress) >= 0.95f;
    }
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount, nearEnd);
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  (void)viewportWidth;
  (void)viewportHeight;

  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  if (heavyBookModeKnown && heavyBookMode) {
    return;
  }

  // Build the next chapter cache a little earlier than the very end.
  // This gives the background job a head start without being too aggressive.
  if (section->currentPage < section->pageCount - 3) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const auto& silentDs = SETTINGS.getDirectionSettings(verticalMode);
  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(verticalMode),
                                  SETTINGS.getReaderLineCompression(verticalMode), silentDs.extraParagraphSpacing,
                                  silentDs.paragraphAlignment, viewportWidth, viewportHeight,
                                  silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, verticalMode, silentDs.charSpacing)) {
    return;
  }

  // NOTE:
  // The concurrent background cache task is currently too risky on some books
  // and can trigger a device abort while the reader is active. Keep the
  // lightweight page prefetch path, but do not start the next-chapter task
  // until the background worker is reworked to be fully safe.
  LOG_DBG("ERS", "Skipping background indexing for next chapter %d to keep reading stable", nextSpineIndex);
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished) {
  FsFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[7];
    data[0] = currentSpineIndex & 0xFF;
    data[1] = (currentSpineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    data[6] = isFinished ? 1 : 0;
    f.write(data, 7);
    f.close();
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d, Finished: %d", spineIndex, currentPage, isFinished);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const int viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const auto t0 = millis();

  // Preload external font glyphs: collect codepoints from page, sort them,
  // and batch-read from SD sequentially. Much faster than random reads during render.
  FontManager& fm = FontManager::getInstance();
  if (fm.isExternalFontEnabled()) {
    ExternalFont* extFont = fm.getActiveFont();
    if (extFont) {
      std::vector<uint32_t> codepoints;
      page->collectCodepoints(codepoints, extFont->getCacheCapacity());
      if (!codepoints.empty()) {
        extFont->preloadGlyphs(codepoints.data(), codepoints.size());
      }
    }
  }

  // Apply vertical character spacing setting for this render
  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
               viewportWidth);  // scan pass
  scope.endScanAndPrewarm();
  if (fcm) {
    fcm->logStats("epub-prewarm");
  }
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing;
  const bool nearChapterEnd = shouldUseSlowRefreshForCurrentPage();

  page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop, viewportWidth);
  renderStatusBar();
  const auto tBwRender = millis();

  displayRenderedPage(*page, orientedMarginLeft, orientedMarginTop, viewportWidth, imagePageWithAA, nearChapterEnd);
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  renderer.storeBwBuffer();
  const auto tBwStore = millis();

  // Grayscale rendering - skip for external fonts (1-bit bitmap, no antialiasing benefit)
  const bool useExternalFont = FontManager::getInstance().isExternalFontEnabled();
  if (SETTINGS.getDirectionSettings(verticalMode).textAntiAliasing && !useExternalFont) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                 viewportWidth);
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                 viewportWidth);
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
            tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tBwRestore - tBwStore,
            tEnd - t0);
  }
}

void EpubReaderActivity::displayRenderedPage(const Page& page, const int orientedMarginLeft,
                                             const int orientedMarginTop, const int viewportWidth,
                                             const bool imagePageWithAA, const bool nearChapterEnd) {
  // Double FAST_REFRESH with selective image blanking (pablohc's technique):
  // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
  // Instead, blank only the image area and do two fast refreshes.
  // Step 1: Display page with image area blanked (text appears, image area white)
  // Step 2: Re-render with images and display again (images appear clean)
  if (imagePageWithAA) {
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page.render(renderer, SETTINGS.getReaderFontId(verticalMode), orientedMarginLeft, orientedMarginTop,
                  viewportWidth);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  // Keep the slower refresh for chapter boundaries only so the body stays responsive.
  renderer.displayBuffer(nearChapterEnd ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

bool EpubReaderActivity::shouldUseSlowRefreshForCurrentPage() const {
  if (!section) {
    return false;
  }
  // Tiny chapters (1-2 pages) already feel "end-like" and do not benefit much from
  // a slow refresh, so keep the heavy refresh reserved for real multi-page chapters.
  return section->pageCount >= 3 && (section->currentPage + 2 >= section->pageCount);
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, verticalMode);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    cancelBackgroundCacheGeneration();
    resetTransientReaderState(false);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    cancelBackgroundCacheGeneration();
    resetTransientReaderState(false);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

