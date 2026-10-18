//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/Slice.h"
#include "td/utils/Span.h"

namespace telegram_bot_api {

// Builds a page in the Prometheus text exposition format. Each metric family must be declared once before its samples
// are added. Families are written in the order of declaration with all their samples together; families without
// samples are omitted.
class MetricsWriter {
 public:
  enum class Type : td::int8 { Counter, Gauge, Histogram };

  void declare(td::Slice name, Type type, td::Slice help);

  // labels are written as is, for example, 'bot="123",method="getMe"'; use escape_label_value for arbitrary values
  void add(td::Slice name, td::Slice labels, td::int64 value);

  void add(td::Slice name, td::Slice labels, double value);

  // counts[i] is the number of observations in the bucket i, which isn't cumulative; the last bucket is +Inf,
  // so counts must have one more element than bounds
  void add_histogram(td::Slice name, td::Slice labels, td::Span<double> bounds, td::Span<td::uint64> counts,
                     double sum);

  td::string finish();

  static td::string escape_label_value(td::Slice value);

 private:
  struct Family {
    td::string name;
    Type type;
    td::string help;
    td::string samples;
  };
  td::vector<Family> families_;
  td::FlatHashMap<td::string, std::size_t> family_indexes_;

  Family &get_family(td::Slice name);

  void add_sample(td::string &samples, td::Slice name, td::Slice suffix, td::Slice labels, td::Slice value);
};

}  // namespace telegram_bot_api
