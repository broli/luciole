#include "fs/file_filter.h"
#include "test_check.h"

#include <exception>
#include <print>
#include <vector>

namespace {
  using luciole::FileFilter;
  using luciole::FilePattern;
  using Kind = luciole::FilePattern::Kind;

  void testGlobs() {
    const FileFilter png(std::vector<FilePattern>{{.kind = Kind::Glob, .pattern = "*.png"}});
    TEST_CHECK(png.matches("a.png", nullptr));
    TEST_CHECK(!png.matches("a.PNG", nullptr));
    const FileFilter folded(std::vector<FilePattern>{{.kind = Kind::Glob, .pattern = "*.[pP][nN][gG]"}});
    TEST_CHECK(folded.matches("a.PNG", nullptr) && folded.matches("a.png", nullptr));
    TEST_CHECK(!FileFilter(std::vector<FilePattern>{}).matches("a.png", nullptr));
  }

  void testMimeTypes() {
    const FileFilter png(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "image/png"}});
    TEST_CHECK(png.matches("x.png", nullptr));
    TEST_CHECK(!png.matches("notes.txt", nullptr));
    const FileFilter images(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "image/*"}});
    TEST_CHECK(images.matches("photo", "image/jpeg"));
    TEST_CHECK(!images.matches("photo.jpg", "text/plain"));
    const FileFilter text(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "text/plain"}});
    TEST_CHECK(text.matches("main.c", "text/x-csrc"));
    const FileFilter either(
        std::vector<FilePattern>{
            {.kind = Kind::Glob, .pattern = "*.txt"}, {.kind = Kind::MimeType, .pattern = "image/png"}
        }
    );
    TEST_CHECK(either.matches("notes.txt", nullptr) && either.matches("x.png", nullptr));
  }

  void testGlobExtensions() {
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.png"}}).primaryExtension() == ".png");
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.PNG"}}).primaryExtension() == ".PNG");
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.tar.gz"}}).primaryExtension() == ".tar.gz");
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.[pP][nN][gG]"}}).primaryExtension() == ".png");
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.[jJ][pP][eE][gG]"}}).primaryExtension() == ".jpeg");

    for (const char* wildcard : {"*", "*.*", "*.?", "*.[ch]", "foo*.png", "*.", "*..png"}) {
      TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = wildcard}}).primaryExtension().empty());
    }
    TEST_CHECK(FileFilter({{.kind = Kind::Glob, .pattern = "*.png"}, {.kind = Kind::Glob, .pattern = "*"}})
                   .primaryExtension()
                   .empty());

    const FileFilter jpeg(
        std::vector<FilePattern>{{.kind = Kind::Glob, .pattern = "*.jpg"}, {.kind = Kind::Glob, .pattern = "*.jpeg"}}
    );
    TEST_CHECK(jpeg.primaryExtension() == ".jpg");
    TEST_CHECK(jpeg.matchedExtensionLength("photo.jpg") == 4);
    TEST_CHECK(jpeg.matchedExtensionLength("photo.JPEG") == 5);
    TEST_CHECK(jpeg.matchedExtensionLength(".jpg") == 0);

    const FileFilter archive(
        std::vector<FilePattern>{{.kind = Kind::Glob, .pattern = "*.gz"}, {.kind = Kind::Glob, .pattern = "*.tar.gz"}}
    );
    TEST_CHECK(archive.matchedExtensionLength("backup.tar.gz") == 7);
  }

  void testMimeExtensions() {
    const FileFilter mimePng(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "image/png"}});
    TEST_CHECK(mimePng.primaryExtension() == ".png");
    TEST_CHECK(mimePng.matchedExtensionLength("icon.png") == 4);

    const FileFilter mimeJpeg(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "image/jpeg"}});
    TEST_CHECK(mimeJpeg.matchedExtensionLength("photo.jpg") == 4);
    TEST_CHECK(mimeJpeg.matchedExtensionLength("photo.jpeg") == 5);

    const FileFilter mimeAllImages(std::vector<FilePattern>{{.kind = Kind::MimeType, .pattern = "image/*"}});
    TEST_CHECK(mimeAllImages.primaryExtension().empty());
    TEST_CHECK(mimeAllImages.matchedExtensionLength("photo.png") == 0);

    TEST_CHECK(luciole::knownExtensionLength("file.png") == 4);
    TEST_CHECK(luciole::knownExtensionLength("file.7z") == 3);
    TEST_CHECK(luciole::knownExtensionLength("archive.tar.gz") == 7);
    TEST_CHECK(luciole::knownExtensionLength("archive.tar.lz") == 7);
    TEST_CHECK(luciole::knownExtensionLength(".tar.gz") == 3);
    TEST_CHECK(luciole::knownExtensionLength(".bashrc.png") == 4);
    TEST_CHECK(luciole::knownExtensionLength(".png") == 0);
  }
} // namespace

int main() {
  try {
    testGlobs();
    testMimeTypes();
    testGlobExtensions();
    testMimeExtensions();
    std::println("file filter checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "file filter test: {}", error.what());
    return 1;
  }
}
