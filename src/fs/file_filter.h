#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  struct FilePattern {
    enum class Kind : std::uint8_t { Glob = 0, MimeType = 1 };
    Kind kind = Kind::Glob;
    std::string pattern;
    bool operator==(const FilePattern&) const = default;
  };

  // FileChooser portal filter semantics: case-sensitive globs, MIME types with subclassing and `type/*`.
  class FileFilter {
  public:
    explicit FileFilter(std::vector<FilePattern> patterns);
    // `contentType`: the entry's known content type, nullptr when unknown (then guessed from `name`).
    bool matches(std::string_view name, const char* contentType) const;
    // The primary `.ext` (such as `.png` or `.tar.gz`) suggested by the filter's patterns, or empty when the filter
    // is a wildcard (`*`, `image/*`) or has no concrete extension.
    std::string primaryExtension() const;
    // Length of the trailing `.ext` in `name` matched by one of this filter's concrete patterns, or 0.
    std::size_t matchedExtensionLength(std::string_view name) const;
    std::size_t patternCount() const noexcept { return m_patterns.size(); }

  private:
    std::vector<FilePattern> m_patterns;
  };

  // Length of any trailing known file extension (including leading `.`, e.g. `.tar.gz` or `.7z`) on `name`, or 0.
  std::size_t knownExtensionLength(std::string_view name);
} // namespace luciole
