#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>

#include <Epub/Page.h>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool verticalMode = false;  // resolved effective writing mode for current book
  bool heavyBookMode = false;
  bool heavyBookModeKnown = false;
  bool backgroundCacheRequested = false;
  bool backgroundCacheRunning = false;
  TaskHandle_t backgroundCacheTaskHandle = nullptr;
  std::shared_ptr<std::atomic_bool> backgroundCacheCancelFlag = std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<std::atomic_bool> backgroundCacheRunningFlag = std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<std::atomic_int> backgroundCacheProgressPercent = std::make_shared<std::atomic_int>(0);
  std::unique_ptr<Page> prefetchedNextPage = nullptr;
  int prefetchedNextPageSpineIndex = -1;
  int prefetchedNextPageNumber = -1;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void displayRenderedPage(const Page& page, int orientedMarginLeft, int orientedMarginTop, int viewportWidth,
                           bool imagePageWithAA, bool nearChapterEnd);
  void renderStatusBar() const;
  bool shouldUseSlowRefreshForCurrentPage() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished = false);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void invalidateSectionPreservingPosition();
  void openChapterSelection();
  void openFootnotes();
  void openPercentSelection();
  void openReaderSettings();
  void displayPageQr();
  void clearCacheAndGoHome();
  void syncReaderState();
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void refreshBackgroundCacheTaskHandle();
  bool handleAutomaticPageTurn();
  bool handleReaderMenuOpen();
  bool handleBackNavigation();
  bool handlePageTurnInput();
  void pageTurn(bool isForwardTurn);
  void pregenerateCache();
  void loadHeavyBookMode();
  void markHeavyBookMode();
  void clearPrefetchedNextPage();
  void resetTransientReaderState(bool cancelBackgroundCache = false);
  std::unique_ptr<Page> loadPageForRender();
  bool shouldPrefetchNextPage() const;
  void prefetchNextPageIfHelpful(uint16_t viewportWidth, uint16_t viewportHeight);
  void startBackgroundCacheGeneration(int startSpineIndex, int endSpineIndex, bool includeImages,
                                      uint16_t viewportWidth, uint16_t viewportHeight);
  void cancelBackgroundCacheGeneration();
  static void backgroundCacheTaskTrampoline(void* param);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  bool supportsLandscape() const override { return true; }
};
