#include "fs/file_filter.h"

#include "fs/gio_ptr.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <gio/gio.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
  bool asciiCaseEqual(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  }

  bool equalsIgnoreCase(std::string_view a, std::string_view b) { return std::ranges::equal(a, b, asciiCaseEqual); }

  bool endsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && equalsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
  }

  // Extracts a concrete `.ext` suffix from a glob pattern such as `*.png`, `*.tar.gz`, or `*.[pP][nN][gG]`.
  std::string globExtension(std::string_view pattern) {
    if (!pattern.starts_with("*.") || pattern.size() < 3) {
      return {};
    }
    std::string ext = ".";
    for (std::size_t i = 2; i < pattern.size(); ++i) {
      const char ch = pattern[i];
      if (ch == '[') {
        if (i + 3 < pattern.size() && pattern[i + 3] == ']') {
          const auto first = static_cast<unsigned char>(pattern[i + 1]);
          const auto second = static_cast<unsigned char>(pattern[i + 2]);
          if (std::isalpha(first) != 0 && std::tolower(first) == std::tolower(second)) {
            ext.push_back(static_cast<char>(std::tolower(first)));
            i += 3;
            continue;
          }
        }
        return {};
      }
      if (ch == '*' || ch == '?' || ch == ']' || ch == '/' || ch == '\\') {
        return {};
      }
      if (ch == '.' && ext.back() == '.') {
        return {};
      }
      ext.push_back(ch);
    }
    if (ext.size() <= 1 || ext.back() == '.') {
      return {};
    }
    return ext;
  }

  bool isCatchAllPattern(const luciole::FilePattern& pattern) {
    if (pattern.kind == luciole::FilePattern::Kind::Glob) {
      return pattern.pattern == "*" || pattern.pattern == "*.*";
    }
    return pattern.pattern.ends_with("/*");
  }

  struct MimeGlobEntry {
    int weight = 0;
    std::string extension;
  };

  struct AsciiCaseInsensitiveHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
      std::size_t hash = 14695981039346656037ULL;
      for (const char ch : value) {
        hash ^= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)));
        hash *= 1099511628211ULL;
      }
      return hash;
    }
  };

  struct AsciiCaseInsensitiveEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return equalsIgnoreCase(a, b); }
  };

  using CaseInsensitiveSet = std::unordered_set<std::string, AsciiCaseInsensitiveHash, AsciiCaseInsensitiveEqual>;
  template <typename T>
  using CaseInsensitiveMap = std::unordered_map<std::string, T, AsciiCaseInsensitiveHash, AsciiCaseInsensitiveEqual>;

  struct MimeGlobTable {
    CaseInsensitiveMap<std::vector<MimeGlobEntry>> globs;
    CaseInsensitiveMap<std::string> aliases;
    CaseInsensitiveSet knownExtensions;
  };

  void loadMimeDir(const std::filesystem::path& mimeDir, MimeGlobTable& table) {
    if (std::ifstream aliasesStream(mimeDir / "aliases"); aliasesStream) {
      std::string alias;
      std::string canonical;
      while (aliasesStream >> alias >> canonical) {
        table.aliases.try_emplace(std::move(alias), std::move(canonical));
      }
    }
    std::ifstream globsStream(mimeDir / "globs2");
    if (!globsStream) {
      return;
    }
    std::string line;
    while (std::getline(globsStream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      const std::string_view view(line);
      const auto firstColon = view.find(':');
      if (firstColon == std::string_view::npos) {
        continue;
      }
      const auto secondColon = view.find(':', firstColon + 1);
      if (secondColon == std::string_view::npos) {
        continue;
      }
      int weight = 0;
      const auto weightView = view.substr(0, firstColon);
      const auto [ptr, ec] = std::from_chars(weightView.data(), weightView.data() + weightView.size(), weight);
      if (ec != std::errc{} || ptr != weightView.data() + weightView.size()) {
        continue;
      }
      const auto mime = view.substr(firstColon + 1, secondColon - firstColon - 1);
      const auto thirdColon = view.find(':', secondColon + 1);
      const auto pattern = thirdColon == std::string_view::npos
          ? view.substr(secondColon + 1)
          : view.substr(secondColon + 1, thirdColon - secondColon - 1);
      std::string ext = globExtension(pattern);
      if (ext.empty()) {
        continue;
      }
      table.knownExtensions.insert(ext);
      auto found = table.globs.find(mime);
      if (found == table.globs.end()) {
        found = table.globs.emplace(std::string(mime), std::vector<MimeGlobEntry>{}).first;
      }
      auto& entries = found->second;
      const auto existing = std::ranges::find_if(entries, [&](const MimeGlobEntry& entry) {
        return equalsIgnoreCase(entry.extension, ext);
      });
      if (existing == entries.end()) {
        entries.push_back({.weight = weight, .extension = std::move(ext)});
      }
    }
  }

  const MimeGlobTable& mimeGlobTable() {
    static const MimeGlobTable table = [] {
      MimeGlobTable loaded;
      if (const char* userDir = g_get_user_data_dir()) {
        loadMimeDir(std::filesystem::path(userDir) / "mime", loaded);
      }
      if (const char* const* systemDirs = g_get_system_data_dirs()) {
        for (const char* const* dir = systemDirs; *dir != nullptr; ++dir) {
          loadMimeDir(std::filesystem::path(*dir) / "mime", loaded);
        }
      }
      return loaded;
    }();
    return table;
  }

  const std::vector<MimeGlobEntry>* mimeGlobsFor(std::string_view mimeType) {
    const auto& table = mimeGlobTable();
    if (const auto found = table.globs.find(mimeType); found != table.globs.end()) {
      return &found->second;
    }
    if (const auto alias = table.aliases.find(mimeType); alias != table.aliases.end()) {
      if (const auto found = table.globs.find(alias->second); found != table.globs.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  std::string primaryMimeExtension(std::string_view mimeType) {
    const auto* entries = mimeGlobsFor(mimeType);
    if (entries == nullptr || entries->empty()) {
      return {};
    }
    const int maxWeight = std::ranges::max(*entries, {}, &MimeGlobEntry::weight).weight;
    const auto slash = mimeType.find('/');
    const std::string_view subtype = slash != std::string_view::npos ? mimeType.substr(slash + 1) : std::string_view{};
    const std::string_view strippedSubtype =
        subtype.starts_with("x-") || subtype.starts_with("X-") ? subtype.substr(2) : std::string_view{};
    const std::string* fallback = nullptr;
    for (const auto& entry : *entries) {
      if (entry.weight != maxWeight) {
        continue;
      }
      if (fallback == nullptr) {
        fallback = &entry.extension;
      }
      const std::string_view extName = std::string_view(entry.extension).substr(1);
      if (equalsIgnoreCase(extName, subtype)
          || (!strippedSubtype.empty() && equalsIgnoreCase(extName, strippedSubtype))) {
        return entry.extension;
      }
    }
    return *fallback;
  }
} // namespace

luciole::FileFilter::FileFilter(std::vector<FilePattern> patterns) : m_patterns(std::move(patterns)) {}

bool luciole::FileFilter::matches(std::string_view name, const char* contentType) const {
  const std::string text(name);
  luciole::GString guessed;
  const auto type = [&] -> const char* {
    if (contentType) {
      return contentType;
    }
    if (!guessed) {
      guessed.reset(g_content_type_guess(text.c_str(), nullptr, 0, nullptr));
    }
    return guessed.get();
  };
  return std::ranges::any_of(m_patterns, [&](const FilePattern& pattern) {
    if (pattern.kind == FilePattern::Kind::Glob) {
      return ::fnmatch(pattern.pattern.c_str(), text.c_str(), 0) == 0;
    }
    const char* actual = type();
    if (!actual) {
      return false;
    }
    if (pattern.pattern.ends_with("/*")) {
      const std::string_view prefix(pattern.pattern.data(), pattern.pattern.size() - 1);
      return std::string_view(actual).starts_with(prefix);
    }
    return g_content_type_is_a(actual, pattern.pattern.c_str()) != FALSE;
  });
}

std::string luciole::FileFilter::primaryExtension() const {
  if (std::ranges::any_of(m_patterns, isCatchAllPattern)) {
    return {};
  }
  for (const auto& pattern : m_patterns) {
    if (pattern.kind == FilePattern::Kind::Glob) {
      if (std::string ext = globExtension(pattern.pattern); !ext.empty()) {
        return ext;
      }
    }
  }
  for (const auto& pattern : m_patterns) {
    if (pattern.kind == FilePattern::Kind::MimeType) {
      if (std::string ext = primaryMimeExtension(pattern.pattern); !ext.empty()) {
        return ext;
      }
    }
  }
  return {};
}

std::size_t luciole::FileFilter::matchedExtensionLength(std::string_view name) const {
  std::size_t best = 0;
  for (const auto& pattern : m_patterns) {
    if (pattern.kind == FilePattern::Kind::Glob) {
      const std::string ext = globExtension(pattern.pattern);
      if (!ext.empty() && name.size() > ext.size() && endsWithIgnoreCase(name, ext)) {
        best = std::max(best, ext.size());
      }
      continue;
    }
    if (isCatchAllPattern(pattern)) {
      continue;
    }
    if (const auto* entries = mimeGlobsFor(pattern.pattern)) {
      for (const auto& entry : *entries) {
        if (name.size() > entry.extension.size() && endsWithIgnoreCase(name, entry.extension)) {
          best = std::max(best, entry.extension.size());
        }
      }
    }
  }
  return best;
}

std::size_t luciole::knownExtensionLength(std::string_view name) {
  const auto lastDot = name.rfind('.');
  if (lastDot == std::string_view::npos || lastDot == 0 || lastDot + 1 >= name.size()) {
    return 0;
  }
  const auto& known = mimeGlobTable().knownExtensions;
  if (const auto prevDot = name.rfind('.', lastDot - 1); prevDot != std::string_view::npos && prevDot > 0) {
    if (const std::string_view compound = name.substr(prevDot); known.contains(compound)) {
      return compound.size();
    }
  }
  const std::string_view suffix = name.substr(lastDot);
  return known.contains(suffix) ? suffix.size() : 0;
}
