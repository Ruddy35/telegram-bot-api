//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/MetricsWriter.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/SliceBuilder.h"

#include <cstdio>

namespace telegram_bot_api {

namespace {
td::string format_double(double value, const char *format) {
  char buf[64];
  auto len = std::snprintf(buf, sizeof(buf), format, value);
  CHECK(len > 0 && static_cast<std::size_t>(len) < sizeof(buf));
  return td::string(buf, static_cast<std::size_t>(len));
}
}  // namespace

void MetricsWriter::declare(td::Slice name, Type type, td::Slice help) {
  CHECK(family_indexes_.count(name.str()) == 0);
  family_indexes_.emplace(name.str(), families_.size());
  families_.push_back(Family{name.str(), type, help.str(), td::string()});
}

MetricsWriter::Family &MetricsWriter::get_family(td::Slice name) {
  auto it = family_indexes_.find(name.str());
  LOG_CHECK(it != family_indexes_.end()) << "Metric " << name << " wasn't declared";
  return families_[it->second];
}

void MetricsWriter::add_sample(td::string &samples, td::Slice name, td::Slice suffix, td::Slice labels,
                               td::Slice value) {
  samples.append(name.begin(), name.size());
  samples.append(suffix.begin(), suffix.size());
  if (!labels.empty()) {
    samples += '{';
    samples.append(labels.begin(), labels.size());
    samples += '}';
  }
  samples += ' ';
  samples.append(value.begin(), value.size());
  samples += '\n';
}

void MetricsWriter::add(td::Slice name, td::Slice labels, td::int64 value) {
  auto &family = get_family(name);
  CHECK(family.type != Type::Histogram);
  add_sample(family.samples, name, td::Slice(), labels, td::to_string(value));
}

void MetricsWriter::add(td::Slice name, td::Slice labels, double value) {
  auto &family = get_family(name);
  CHECK(family.type != Type::Histogram);
  add_sample(family.samples, name, td::Slice(), labels, format_double(value, "%.6f"));
}

void MetricsWriter::add_histogram(td::Slice name, td::Slice labels, td::Span<double> bounds,
                                  td::Span<td::uint64> counts, double sum) {
  auto &family = get_family(name);
  CHECK(family.type == Type::Histogram);
  CHECK(counts.size() == bounds.size() + 1);
  td::string separator = labels.empty() ? td::string() : td::string(",");
  td::uint64 cumulative_count = 0;
  for (std::size_t i = 0; i < counts.size(); i++) {
    cumulative_count += counts[i];
    auto le = i < bounds.size() ? format_double(bounds[i], "%g") : td::string("+Inf");
    add_sample(family.samples, name, "_bucket", PSLICE() << labels << separator << "le=\"" << le << '"',
               td::to_string(cumulative_count));
  }
  add_sample(family.samples, name, "_sum", labels, format_double(sum, "%.6f"));
  add_sample(family.samples, name, "_count", labels, td::to_string(cumulative_count));
}

td::string MetricsWriter::finish() {
  static const char *const TYPE_NAMES[] = {"counter", "gauge", "histogram"};
  td::string result;
  for (auto &family : families_) {
    if (family.samples.empty()) {
      continue;
    }
    result += PSTRING() << "# HELP " << family.name << ' ' << family.help << "\n# TYPE " << family.name << ' '
                        << TYPE_NAMES[static_cast<int>(family.type)] << '\n';
    result += family.samples;
  }
  families_.clear();
  family_indexes_.clear();
  return result;
}

td::string MetricsWriter::escape_label_value(td::Slice value) {
  td::string result;
  result.reserve(value.size());
  for (auto c : value) {
    switch (c) {
      case '\\':
        result += "\\\\";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\n':
        result += "\\n";
        break;
      default:
        result += c;
    }
  }
  return result;
}

}  // namespace telegram_bot_api
