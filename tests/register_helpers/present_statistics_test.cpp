// Display-side pacing accumulator (rex/ui/present_statistics.h): missed
// refreshes are counted from displayed images vs refreshes, not inferred from
// renderer timing.
#include <rex/ui/present_statistics.h>

#include <iostream>

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  using rex::ui::PresentStatisticsWindow;

  {
    // Every image on screen for one refresh: no misses.
    PresentStatisticsWindow w;
    w.Sample(100, 1000, 1, 1);  // baseline
    for (uint32_t i = 1; i <= 10; ++i) w.Sample(100 + i, 1000 + i, int64_t(i), 1);
    check(w.displayed == 10 && w.refreshes == 10 && w.missed == 0 && w.glitches == 0 &&
              w.max_refreshes_per_image == 1,
          "one refresh per image at interval 1");
  }
  {
    // One image repeated for an extra refresh, and a batch of two images over
    // three refreshes.
    PresentStatisticsWindow w;
    w.Sample(10, 50, 0, 1);
    w.Sample(11, 51, 1, 1);
    w.Sample(12, 53, 2, 1);  // image 11 stayed two refreshes
    w.Sample(12, 53, 3, 1);  // nothing new displayed yet
    w.Sample(14, 56, 4, 1);  // two images over three refreshes
    check(w.displayed == 4 && w.refreshes == 6 && w.missed == 2 && w.glitches == 2 &&
              w.max_refreshes_per_image == 2 && w.miss_records == 2 &&
              w.misses[0].refreshes == 2 && w.misses[0].sync_qpc == 2,
          "extra refreshes are missed refreshes");
  }
  {
    // Interval 2 (half refresh): two refreshes per image are expected.
    PresentStatisticsWindow w;
    w.Sample(1, 10, 0, 2);
    w.Sample(2, 12, 1, 2);
    w.Sample(3, 15, 2, 2);
    check(w.displayed == 2 && w.missed == 1, "interval 2 expects two refreshes per image");
    // Tearing presents carry no refresh expectation.
    PresentStatisticsWindow t;
    t.Sample(1, 10, 0, 0);
    t.Sample(5, 11, 1, 0);
    check(t.displayed == 4 && t.missed == 0, "immediate presents are counted without misses");
  }
  {
    // Disjoint statistics rebaseline; Reset keeps the baseline across windows.
    PresentStatisticsWindow w;
    w.Sample(1, 10, 0, 1);
    w.Disjoint();
    w.Sample(50, 900, 1, 1);  // new baseline, not a 49-image jump
    check(w.displayed == 0 && w.disjoint == 1, "disjoint rebaselines");
    w.Sample(51, 901, 2, 1);
    w.CountPresent(2);
    w.Reset(1234);
    check(w.begin_qpc == 1234 && w.displayed == 0 && w.presents == 0 && w.have_last &&
              w.last_present_count == 51,
          "reset keeps the displayed-image baseline");
    w.Sample(52, 903, 3, 1);
    check(w.displayed == 1 && w.missed == 1, "baseline carries into the next window");
  }

  {
    // Scheduler-side accounting: refreshes elapsed vs images displayed.
    PresentStatisticsWindow w;
    w.SyncSample(100, 7, 1000, 10);
    w.SyncSample(172, 7, 1072, 20);  // 72 refreshes, 72 new images
    check(w.SyncRefreshes() == 72 && w.SyncDisplayed() == 72 && w.SyncMissed(1) == 0,
          "one new image per refresh");
    w.SyncSample(242, 7, 1144, 30);  // 72 more refreshes, 70 images
    check(w.SyncRefreshes() == 144 && w.SyncDisplayed() == 142 && w.SyncMissed(1) == 2 &&
              w.SyncMissed(0) == 0,
          "refreshes without a new image are missed; tearing has no expectation");
    w.Reset(99);
    check(w.have_sync && w.SyncRefreshes() == 0 && w.first_sync_refresh == 1144,
          "the next window starts where this one ended");
    w.SyncSample(278, 7, 1216, 40);
    check(w.SyncRefreshes() == 72 && w.SyncMissed(2) == 0, "interval 2: 36 images over 72");
  }

  {
    // Compositor vblank counter vs displayed images.
    PresentStatisticsWindow w;
    w.DwmSample(10, 5000, 69444);
    w.DwmSample(82, 5072, 69444);
    check(w.DwmRefreshes() == 72 && w.DwmDisplayed() == 72 && w.DwmMissed(1) == 0,
          "one image per compositor refresh");
    w.DwmSample(150, 5144, 69444);
    check(w.DwmMissed(1) == 4 && w.DwmMissed(0) == 0, "four refreshes without a new image");
    w.Reset(1);
    w.DwmSample(186, 5216, 69444);
    check(w.DwmRefreshes() == 72 && w.DwmDisplayed() == 36 && w.DwmMissed(2) == 0,
          "window restarts at the last sample; interval 2");
  }

  if (!passed) return 1;
  std::cout << "present statistics: PASS\n";
  return 0;
}
