#pragma once

// Minimal command-line parsing shared by the tools: `--name value`, `--name=value` and boolean
// `--flag`. Intentionally tiny; the tools have a handful of options each.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dslr::tools {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg.rfind("--", 0) != 0) {
        positional_.push_back(arg);
        continue;
      }
      arg = arg.substr(2);
      const auto eq = arg.find('=');
      if (eq != std::string::npos) {
        values_[arg.substr(0, eq)] = arg.substr(eq + 1);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        values_[arg] = argv[++i];
      } else {
        values_[arg] = "true";
      }
    }
  }

  bool has(const std::string& name) const { return values_.count(name) != 0; }
  bool flag(const std::string& name) const {
    const auto it = values_.find(name);
    return it != values_.end() && it->second != "false" && it->second != "0";
  }

  std::string get(const std::string& name, const std::string& fallback) const {
    const auto it = values_.find(name);
    return it == values_.end() ? fallback : it->second;
  }

  template <typename T>
  T number(const std::string& name, T fallback) const {
    const auto it = values_.find(name);
    if (it == values_.end()) {
      return fallback;
    }
    try {
      if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(std::stod(it->second));
      } else {
        return static_cast<T>(std::stoll(it->second));
      }
    } catch (const std::exception&) {
      throw std::invalid_argument("--" + name + " expects a number, got '" + it->second + "'");
    }
  }

  /// Durations accept a unit suffix: "5us", "10ms", "2s", "1m". Plain numbers are microseconds.
  std::chrono::microseconds duration(const std::string& name,
                                     std::chrono::microseconds fallback) const {
    const auto it = values_.find(name);
    if (it == values_.end()) {
      return fallback;
    }
    return parse_duration(it->second, name);
  }

  const std::vector<std::string>& positional() const { return positional_; }

  static std::chrono::microseconds parse_duration(const std::string& text,
                                                  const std::string& name) {
    size_t consumed = 0;
    double value = 0;
    try {
      value = std::stod(text, &consumed);
    } catch (const std::exception&) {
      throw std::invalid_argument("--" + name + " expects a duration, got '" + text + "'");
    }
    const std::string unit = text.substr(consumed);
    double factor = 1.0;  // microseconds
    if (unit == "ns") {
      factor = 1e-3;
    } else if (unit == "us" || unit.empty()) {
      factor = 1.0;
    } else if (unit == "ms") {
      factor = 1e3;
    } else if (unit == "s") {
      factor = 1e6;
    } else if (unit == "m") {
      factor = 60e6;
    } else {
      throw std::invalid_argument("--" + name + ": unknown time unit '" + unit + "'");
    }
    return std::chrono::microseconds(static_cast<long long>(value * factor));
  }

 private:
  std::map<std::string, std::string> values_;
  std::vector<std::string> positional_;
};

/// Splits "host:port,host:port" into pairs.
inline std::vector<std::pair<std::string, uint16_t>> parse_host_ports(const std::string& list,
                                                                      uint16_t default_port) {
  std::vector<std::pair<std::string, uint16_t>> result;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    const std::string item = list.substr(start, end - start);
    if (!item.empty()) {
      const auto colon = item.rfind(':');
      if (colon == std::string::npos) {
        result.emplace_back(item, default_port);
      } else {
        result.emplace_back(item.substr(0, colon),
                            static_cast<uint16_t>(std::stoi(item.substr(colon + 1))));
      }
    }
    start = end + 1;
  }
  return result;
}

}  // namespace dslr::tools
