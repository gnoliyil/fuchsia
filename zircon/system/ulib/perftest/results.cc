// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <zircon/assert.h>

#include <algorithm>
#include <numeric>
#include <utility>

#include <perftest/results.h>

namespace perftest {
namespace {

double Mean(const fbl::Vector<double>& values) {
  double sum = std::accumulate(values.begin(), values.end(), 0.0);
  return sum / static_cast<double>(values.size());
}

double Min(const fbl::Vector<double>& values) {
  return *std::min_element(values.begin(), values.end());
}

double Max(const fbl::Vector<double>& values) {
  return *std::max_element(values.begin(), values.end());
}

double StdDev(const fbl::Vector<double>& values, double mean) {
  double sum_of_squared_diffs = 0.0;
  for (double value : values) {
    double diff = value - mean;
    sum_of_squared_diffs += diff * diff;
  }
  return sqrt(sum_of_squared_diffs / static_cast<double>(values.size()));
}

// Comparison function for use with qsort().
int CompareDoubles(const void* ptr1, const void* ptr2) {
  double val1 = *reinterpret_cast<const double*>(ptr1);
  double val2 = *reinterpret_cast<const double*>(ptr2);
  if (val1 < val2) {
    return -1;
  }
  if (val1 > val2) {
    return 1;
  }
  return 0;
}

double Median(const fbl::Vector<double>& values) {
  // Make a sorted copy of the vector.
  fbl::Vector<double> copy;
  copy.reserve(values.size());
  for (double value : values) {
    copy.push_back(value);
  }
  qsort(copy.data(), copy.size(), sizeof(copy[0]), CompareDoubles);

  size_t index = copy.size() / 2;
  // Interpolate two values if necessary.
  if (copy.size() % 2 == 0) {
    return (copy[index - 1] + copy[index]) / 2;
  }
  return copy[index];
}

}  // namespace

SummaryStatistics TestCaseResults::GetSummaryStatistics() const {
  ZX_ASSERT(values.size() > 0);
  double mean = Mean(values);
  return SummaryStatistics{
      .min = Min(values),
      .max = Max(values),
      .mean = mean,
      .std_dev = StdDev(values, mean),
      .median = Median(values),
  };
}

void WriteJSONString(FILE* out_file, const char* string) {
  fputc('"', out_file);
  for (const char* ptr = string; *ptr; ptr++) {
    uint8_t c = *ptr;
    if (c == '"') {
      fputs("\\\"", out_file);
    } else if (c == '\\') {
      fputs("\\\\", out_file);
    } else if (c < 32 || c >= 128) {
      // Escape non-printable characters (<32) and top-bit-set
      // characters (>=128).
      //
      // TODO(fxbug.dev/7365): Handle top-bit-set characters better.  Ideally
      // we should treat the input string as UTF-8 and preserve the
      // encoded Unicode in the JSON.  We could interpret the UTF-8
      // sequences and convert them to \uXXXX escape sequences.
      // Alternatively we could pass through UTF-8, but if we do
      // that, we ought to block overlong UTF-8 sequences to prevent
      // closing quotes from being encoded as overlong UTF-8
      // sequences.
      //
      // The current code treats the input string as a byte array
      // rather than UTF-8, which isn't *necessarily* what we want,
      // but will at least result in valid JSON and make the data
      // recoverable.
      fprintf(out_file, "\\u%04x", c);
    } else {
      fputc(c, out_file);
    }
  }
  fputc('"', out_file);
}

void TestCaseResults::WriteJSON(FILE* out_file) const {
  fprintf(out_file, "{\"label\":");
  WriteJSONString(out_file, label.c_str());
  fprintf(out_file, ",\"test_suite\":");
  WriteJSONString(out_file, test_suite.c_str());
  fprintf(out_file, ",\"unit\":");
  WriteJSONString(out_file, unit.c_str());
  fprintf(out_file, ",\"values\":[");
  bool first = true;
  for (const auto value : values) {
    if (!first) {
      fprintf(out_file, ",");
    }
    fprintf(out_file, "%f", value);
    first = false;
  }
  fprintf(out_file, "]}");
}

TestCaseResults* ResultsSet::AddTestCase(const fbl::String& test_suite, const fbl::String& label,
                                         const fbl::String& unit) {
  TestCaseResults test_case(test_suite, label, unit);
  results_.push_back(std::move(test_case));
  return &results_[results_.size() - 1];
}

void ResultsSet::WriteJSON(FILE* out_file) const {
  fprintf(out_file, "[");
  bool first = true;
  for (const auto& test_case_results : results_) {
    if (!first) {
      fprintf(out_file, ",\n");
    }
    test_case_results.WriteJSON(out_file);
    first = false;
  }
  fprintf(out_file, "]");
}

bool ResultsSet::WriteJSONFile(const char* output_filename) const {
  FILE* fh = fopen(output_filename, "w");
  if (!fh) {
    fprintf(stderr, "Failed to open output file \"%s\": %s\n", output_filename, strerror(errno));
    return false;
  }
  WriteJSON(fh);
  fclose(fh);
  return true;
}

void ResultsSet::PrintSummaryStatistics(FILE* out_file) const {
  // Print table headings row.
  fprintf(out_file, "%10s %10s %10s %10s %10s %-12s %s\n", "Mean", "Std dev", "Min", "Max",
          "Median", "Unit", "Test case");
  if (results_.size() == 0) {
    fprintf(out_file, "(No test results)\n");
  }
  for (const auto& test : results_) {
    SummaryStatistics stats = test.GetSummaryStatistics();
    fprintf(out_file, "%10.0f %10.0f %10.0f %10.0f %10.0f %-12s", stats.mean, stats.std_dev,
            stats.min, stats.max, stats.median, test.unit.c_str());
    fprintf(out_file, " %s\n", test.label.c_str());
  }
}

}  // namespace perftest
