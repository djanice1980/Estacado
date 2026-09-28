#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <rex/graphics/xenos_zpd_report.h>

using rex::graphics::XenosZPDDeferredReports;
using rex::graphics::XenosZPDReport;
using rex::graphics::XenosZPDReportAccumulator;
using rex::graphics::xenos::xe_gpu_depth_sample_counts;

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= Check(XenosZPDReport::GetSlotBase(0x00123467u) == 0x00123440u,
              "slot alignment is incorrect");
  ok &= Check(XenosZPDReport::GetEndRecordBase(0x00123467u) == 0x00123440u,
              "END record ownership is incorrect");
  ok &= Check(XenosZPDReport::GetBeginRecordBase(0x00123467u) == 0x00123460u,
              "BEGIN record ownership is incorrect");
  ok &= Check(XenosZPDReport::IsBeginRecord(0x00123467u),
              "BEGIN record was not recognized");
  ok &= Check(!XenosZPDReport::IsEndRecord(0x00123467u),
              "BEGIN record was misclassified as END");
  ok &= Check(XenosZPDReport::IsEndRecord(0x0012344Cu),
              "END record was not recognized");

  xe_gpu_depth_sample_counts begin{};
  xe_gpu_depth_sample_counts end{};
  XenosZPDReport::WriteReportDelta(&begin, &end, 7, 13);
  ok &= Check(begin.Total_A == 7 && begin.ZPass_A == 7,
              "BEGIN cumulative value is incorrect");
  ok &= Check(end.Total_A == 20 && end.ZPass_A == 20,
              "END cumulative value is incorrect");
  ok &= Check(end.Total_B == 0 && end.ZFail_A == 0 && end.StencilFail_A == 0,
              "unsupported report lanes were not cleared");

  end.ZPass_A = 0xEDFEFFFFu;
  ok &= Check(XenosZPDReport::HasPendingSentinel(&end),
              "little-endian pending sentinel was not recognized");
  end.ZPass_A = 0;
  end.ZFail_A = 0xFFFFFEEDu;
  ok &= Check(XenosZPDReport::HasPendingSentinel(&end),
              "big-endian pending sentinel was not recognized");

  XenosZPDReport::WriteReportDelta(&begin, &end, UINT32_MAX - 2, 20);
  ok &= Check(end.Total_A == UINT32_MAX && end.ZPass_A == UINT32_MAX,
              "cumulative report value did not saturate");

  XenosZPDReportAccumulator logical;
  ok &= Check(logical.Begin(0x00123467u, 41),
              "logical report did not accept its BEGIN record");
  logical.AccumulateSegment(3);
  logical.AccumulateSegment(5);
  logical.AccumulateSegment(7);
  ok &= Check(logical.CanEnd(0x0012344Cu),
              "logical report did not accept its paired END record");
  ok &= Check(!logical.CanEnd(0x0012340Cu),
              "logical report accepted an END from a different slot");
  ok &= Check(logical.accumulated_samples == 15,
              "host query segments were not accumulated across submissions");
  XenosZPDReport::WriteReportDelta(&begin, &end, logical.begin_value,
                                   logical.accumulated_samples);
  ok &= Check(begin.ZPass_A == 41 && end.ZPass_A == 56,
              "split report did not preserve cumulative BEGIN/END values");
  logical.Reset();
  ok &= Check(!logical.valid && logical.accumulated_samples == 0,
              "logical report reset retained a prior lifetime");

  // Publication for a concurrently polled report must produce exactly the
  // bytes of the synchronous writer, including saturation.
  const uint32_t publish_begin_values[] = {0, 7, 41, UINT32_MAX - 2};
  const uint64_t publish_deltas[] = {0, 13, 15, 20, uint64_t(UINT32_MAX) + 9};
  for (uint32_t begin_value : publish_begin_values) {
    for (uint64_t delta : publish_deltas) {
      alignas(64) xe_gpu_depth_sample_counts expected[2];
      alignas(64) xe_gpu_depth_sample_counts published[2];
      std::memset(expected, 0xA5, sizeof(expected));
      std::memset(published, 0xA5, sizeof(published));
      published[0].ZPass_A = 0xEDFEFFFFu;
      published[0].ZPass_B = 0xEDFEFFFFu;
      XenosZPDReport::WriteReportDelta(&expected[1], &expected[0], begin_value,
                                       delta);
      XenosZPDReport::PublishReportDelta(&published[1], &published[0],
                                         begin_value, delta);
      ok &= Check(!std::memcmp(expected, published, sizeof(expected)),
                  "deferred publication differs from the synchronous report");
      ok &= Check(!XenosZPDReport::HasPendingSentinel(&published[0]),
                  "published report retained the pending sentinel");
    }
  }

  // Deferred reports: two reports on different slots, the first spanning two
  // submissions. Nothing is published before every segment has completed,
  // and publication follows END order.
  uint64_t readback[8] = {3, 5, 11, 0, 0, 0, 0, 0};
  auto read_samples = [&](const XenosZPDDeferredReports::Segment& segment) {
    return readback[segment.host_index];
  };
  std::vector<XenosZPDReportAccumulator> published_reports;
  std::vector<uint64_t> published_tags;
  auto publish = [&](const XenosZPDDeferredReports::EndedReport& ended) {
    published_reports.push_back(ended.report);
    published_tags.push_back(ended.tag);
  };
  XenosZPDDeferredReports deferred;
  XenosZPDReportAccumulator open;
  ok &= Check(open.Begin(0x00200020u, 100), "first deferred BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(0, 1, 10);  // closed by a submission boundary
  deferred.AddSegment(1, 1, 11);  // closed at END
  deferred.End(open);
  open.Reset();
  ok &= Check(open.Begin(0x00200060u, 200), "second deferred BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(2, 1, 11);
  deferred.End(open, 77);
  open.Reset();
  ok &= Check(deferred.HasEndedReportInRange(0x00200000u, 0x40) &&
                  deferred.HasEndedReportInRange(0x00200058u, 4) &&
                  !deferred.HasEndedReportInRange(0x00200080u, 0x40),
              "ended report slot overlap is incorrect");
  ok &= Check(deferred.HasEndedReportInRange(0xA0200010u, 4),
              "slot overlap ignored the GPU physical address alias");
  ok &= Check(deferred.AwaitSubmissionForRange(0x00200040u, 0x40) == 11 &&
                  deferred.AwaitSubmissionForRange(0x00200000u, 4) == 11,
              "same-slot await submission is incorrect");
  ok &= Check(deferred.AwaitSubmissionForHostIndex(1) == 11 &&
                  deferred.AwaitSubmissionForHostIndex(5) == 0,
              "host query index ownership is incorrect");
  ok &= Check(deferred.HasSegmentInSubmission(11) &&
                  !deferred.HasSegmentInSubmission(12),
              "open-submission segment detection is incorrect");

  deferred.Retire(9, open, read_samples, publish);
  ok &= Check(published_reports.empty() && deferred.segments.size() == 3,
              "report published before its host segments completed");
  deferred.Retire(10, open, read_samples, publish);
  ok &= Check(published_reports.empty() && deferred.segments.size() == 2,
              "partially completed report was published");
  deferred.Retire(11, open, read_samples, publish);
  ok &= Check(published_reports.size() == 2 && deferred.empty(),
              "completed deferred reports were not all published");
  if (published_reports.size() == 2) {
    ok &= Check(published_reports[0].slot_base == 0x00200000u &&
                    published_reports[0].begin_value == 100 &&
                    published_reports[0].accumulated_samples == 8,
                "first deferred report lost a segment across submissions");
    ok &= Check(published_reports[1].slot_base == 0x00200040u &&
                    published_reports[1].begin_value == 200 &&
                    published_reports[1].accumulated_samples == 11,
                "second deferred report was published incorrectly");
    ok &= Check(published_tags.size() == 2 && published_tags[0] == 0 &&
                    published_tags[1] == 77,
                "deferred report tag was not carried to publication");
  }

  // Segments that complete while their report is still open accumulate into
  // the open report; a report without segments publishes at the next retire.
  published_reports.clear();
  ok &= Check(open.Begin(0x00200020u, 108), "reopened slot BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(3, 1, 12);
  readback[3] = 6;
  deferred.Retire(12, open, read_samples, publish);
  ok &= Check(published_reports.empty() && open.accumulated_samples == 6 &&
                  deferred.open_pending_segments == 0,
              "completed segment of the open report was not accumulated");
  deferred.End(open);
  open.Reset();
  ok &= Check(deferred.AwaitSubmissionForRange(0x00200000u, 0x40) <= 12 &&
                  deferred.HasEndedReportInRange(0x00200000u, 0x40),
              "report without in-flight segments must not await new work");
  deferred.Retire(0, open, read_samples, publish);
  ok &= Check(published_reports.size() == 1 &&
                  published_reports[0].accumulated_samples == 6 &&
                  deferred.empty(),
              "report without in-flight segments was not published");

  // A report without segments that ended behind a pending report must wait
  // for that earlier report: publication stays in END order.
  published_reports.clear();
  ok &= Check(open.Begin(0x00200020u, 114), "ordered BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(5, 1, 20);
  deferred.End(open);
  open.Reset();
  ok &= Check(open.Begin(0x00200060u, 211), "empty ordered BEGIN rejected");
  deferred.Open();
  deferred.End(open);
  open.Reset();
  ok &= Check(deferred.AwaitSubmissionForRange(0x00200040u, 0x40) == 20,
              "same-slot await must include earlier reports in END order");
  deferred.Retire(19, open, read_samples, publish);
  ok &= Check(published_reports.empty(),
              "later report was published ahead of an earlier pending report");
  deferred.Retire(20, open, read_samples, publish);
  ok &= Check(published_reports.size() == 2 &&
                  published_reports[0].slot_base == 0x00200000u &&
                  published_reports[1].slot_base == 0x00200040u &&
                  deferred.empty(),
              "ordered deferred reports were not published in END order");

  // A discarded open report never publishes; its segment still retires so
  // its host index is not reused early.
  published_reports.clear();
  ok &= Check(open.Begin(0x00200060u, 211), "discard BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(4, 1, 13);
  deferred.DiscardOpen();
  open.Reset();
  ok &= Check(deferred.AwaitSubmissionForHostIndex(4) == 13,
              "discarded report released its host index early");
  deferred.Retire(13, open, read_samples, publish);
  ok &= Check(published_reports.empty() && deferred.empty() &&
                  open.accumulated_samples == 0,
              "discarded report was published or accumulated");

  // A new lifetime of a slot supersedes its unpublished earlier results: they
  // retire (host indices free) but never reach guest memory, and no longer
  // make same-slot writes or the lag bound wait. Other slots are unaffected.
  published_reports.clear();
  ok &= Check(open.Begin(0x00200020u, 300), "superseded BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(6, 1, 30);
  deferred.End(open, 5);
  open.Reset();
  ok &= Check(open.Begin(0x00200060u, 400), "unrelated BEGIN rejected");
  deferred.Open();
  deferred.AddSegment(7, 1, 31);
  deferred.End(open, 6);
  open.Reset();
  ok &= Check(deferred.AwaitSubmissionForTag(4) == 0 &&
                  deferred.AwaitSubmissionForTag(5) == 30 &&
                  deferred.AwaitSubmissionForTag(6) == 31,
              "tag-bounded await is incorrect");
  ok &= Check(deferred.SupersedeSlot(0x00200000u) == 1 &&
                  deferred.SupersedeSlot(0x00200000u) == 0,
              "slot supersession count is incorrect");
  ok &= Check(!deferred.HasEndedReportInRange(0x00200000u, 0x40) &&
                  deferred.AwaitSubmissionForRange(0x00200000u, 0x40) == 0 &&
                  deferred.HasEndedReportInRange(0x00200040u, 0x40),
              "superseded report still awaited for its slot");
  ok &= Check(deferred.AwaitSubmissionForTag(5) == 0 &&
                  deferred.AwaitSubmissionForTag(6) == 31,
              "tag-bounded await must skip superseded reports but keep END order");
  ok &= Check(deferred.AwaitSubmissionForHostIndex(6) == 30,
              "superseded report released its host index early");
  readback[6] = 9;
  readback[7] = 4;
  deferred.Retire(31, open, read_samples, publish);
  ok &= Check(published_reports.size() == 1 &&
                  published_reports[0].slot_base == 0x00200040u &&
                  published_reports[0].accumulated_samples == 4 && deferred.empty(),
              "superseded report reached guest memory or blocked a later report");

  // A new lifetime's BEGIN makes its END pending again: an older result may
  // have been published over the sentinel the guest wrote when it issued the
  // query. The ZPass pair gets the guest's own sentinel bytes (0xFFFFFEED
  // stored big-endian); the other lanes are left alone.
  xe_gpu_depth_sample_counts stale{};
  XenosZPDReport::WriteReportDelta(nullptr, &stale, 1000, 50);
  XenosZPDReport::WritePendingSentinel(&stale);
  uint8_t zpass_lanes[8];
  std::memcpy(zpass_lanes, &stale.ZPass_A, sizeof(zpass_lanes));
  const uint8_t guest_sentinel[8] = {0xFF, 0xFF, 0xFE, 0xED, 0xFF, 0xFF, 0xFE, 0xED};
  ok &= Check(!std::memcmp(zpass_lanes, guest_sentinel, sizeof(zpass_lanes)) &&
                  XenosZPDReport::HasPendingSentinel(&stale) && stale.Total_A == 1050,
              "a new lifetime's END was not pending again");
  return ok ? 0 : 1;
}
