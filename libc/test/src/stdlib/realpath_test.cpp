//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unit tests for realpath.
///
//===----------------------------------------------------------------------===//

#include "hdr/errno_macros.h"
#include "hdr/func/free.h"
#include "hdr/limits_macros.h"
#include "hdr/types/size_t.h"
#include "src/__support/CPP/string.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/utility.h"
#include "src/__support/OSUtil/path.h"
#include "src/__support/fixedvector.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/fcntl/openat.h"
#include "src/stdlib/realpath.h"
#include "src/sys/stat/mkdirat.h"
#include "src/unistd/close.h"
#include "src/unistd/unlinkat.h"
#include "test/UnitTest/ErrnoCheckingTest.h"
#include "test/UnitTest/ErrnoSetterMatcher.h"

namespace cpp = LIBC_NAMESPACE::cpp;
namespace path = LIBC_NAMESPACE::path;
using LIBC_NAMESPACE::FixedVector;
using LIBC_NAMESPACE::testing::tlog;
using LIBC_NAMESPACE::testing::ErrnoSetterMatcher::Succeeds;

// This test assumes the following values, so fail early if they mismatch.
static_assert(path::SEPARATOR == '/');
static_assert(path::CURRENT_DIR_COMPONENT == ".");
static_assert(path::PARENT_DIR_COMPONENT == "..");

// Size of a path separator.
constexpr size_t PATH_SEP_SIZE = 1;

// Creates the given directory if it does not exist. Returns zero on success.
[[nodiscard]] int ensure_directory_exists(int dirfd, const char *path,
                                          mode_t mode = 0755) {
  if (LIBC_NAMESPACE::mkdirat(dirfd, path, mode) == 0)
    return 0;
  if (libc_errno == EEXIST)
    libc_errno = 0;

  if (libc_errno != 0) {
    tlog << "Failed to create temp directory: " << path << "\n";
    return -1;
  }
  return 0;
}

// Wrapper for a file descriptor that closes it on destruction.
class FileDescriptor {
  int fd_ = -1;

  void close() {
    if (fd_ >= 0)
      LIBC_NAMESPACE::close(fd_);
    fd_ = -1;
  }

public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  FileDescriptor(FileDescriptor &&other) { *this = cpp::move(other); }
  FileDescriptor &operator=(FileDescriptor &&other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
    return *this;
  }

  ~FileDescriptor() { close(); }

  operator int() const { return fd_; }
};

// A test directory that removes itself on destruction.
class TestDir {
  FileDescriptor fd_;
  cpp::string path_;
  FixedVector<cpp::string, 64> files_;
  FixedVector<cpp::string, 64> dirs_;

public:
  TestDir() = default;
  explicit TestDir(FileDescriptor fd, cpp::string path)
      : fd_(cpp::move(fd)), path_(cpp::move(path)) {}

  ~TestDir() {
    if (path_.empty())
      return;

    // Remove files first.
    for (cpp::string &path : files_)
      LIBC_NAMESPACE::unlinkat(fd_, cpp::move(path).c_str(), 0);

    // Remove directories in reverse order so they'll be empty.
    for (size_t i = dirs_.size(); i > 0; i--)
      LIBC_NAMESPACE::unlinkat(fd_, cpp::move(dirs_[i - 1]).c_str(),
                               AT_REMOVEDIR);

    LIBC_NAMESPACE::unlinkat(AT_FDCWD, path_.c_str(), AT_REMOVEDIR);
  }

  TestDir &operator=(TestDir &&other) = default;

  // Returns the absolute path of `relpath` in this test directory.
  cpp::string abspath(cpp::string_view relpath) const {
    return path_ + "/" + relpath;
  }

  // Returns this test directory path as a C string.
  const char *c_str() const { return path_.c_str(); }

  // Returns this test directory path as a string view.
  const cpp::string_view view() const { return path_; }

  // Creates a directory relative to TestDir. Returns zero on success.
  [[nodiscard]] int mkdir(const char *relpath, mode_t mode = 0755) {
    if (!dirs_.push_back(relpath)) {
      tlog << "Not enough space in TestDir::dirs_\n";
      return -1;
    }

    return ensure_directory_exists(fd_, relpath, mode);
  }

  // Creates an empty file relative to TestDir. Returns zero on success.
  [[nodiscard]] int touch(const char *relpath, mode_t mode = 0644) {
    if (!files_.push_back(relpath)) {
      tlog << "Not enough space in TestDir::files_\n";
      return -1;
    }

    FileDescriptor fd(
        LIBC_NAMESPACE::openat(fd_, relpath, O_RDONLY | O_CREAT, mode));
    return fd < 0 ? -1 : 0;
  }
};

class LlvmLibcRealpathTest : public LIBC_NAMESPACE::testing::ErrnoCheckingTest {
public:
  char *realpath_buffered(const char *path) {
    return LIBC_NAMESPACE::realpath(path, buf_);
  }

  char *realpath_buffered(const cpp::string &path) {
    return realpath_buffered(path.c_str());
  }

  // Creates a test directory in dst. Returns true if successful.
  //
  // While we would prefer to return cpp::optional<TestDir> here,
  // LLVM-libc's optional expects types to be trivially destructible.
  [[nodiscard]] bool create_test_dir(const char *name, TestDir &dst) {
    // Use /tmp instead of the typical libc_make_test_file_path to ensure
    // the path is absolute and does not contain symlinks.
    cpp::string test_dir_path =
        cpp::string("/tmp/LlvmLibcRealpathTest.") + name;
    if (ensure_directory_exists(AT_FDCWD, test_dir_path.c_str()))
      return false;
    FileDescriptor fd(LIBC_NAMESPACE::openat(AT_FDCWD, test_dir_path.c_str(),
                                             O_RDONLY | O_DIRECTORY));
    if (fd < 0)
      return false;

    dst = TestDir(cpp::move(fd), cpp::move(test_dir_path));
    return true;
  }

private:
  char buf_[PATH_MAX];
};

TEST_F(LlvmLibcRealpathTest, ErrorsWithInvalidArgIfNullPath) {
  ASSERT_EQ(realpath_buffered(nullptr), nullptr);
  ASSERT_ERRNO_EQ(EINVAL);
}

TEST_F(LlvmLibcRealpathTest, ErrorsWithNoEntryIfEmptyPath) {
  ASSERT_EQ(realpath_buffered(""), nullptr);
  ASSERT_ERRNO_EQ(ENOENT);
}

TEST_F(LlvmLibcRealpathTest, OkIfPathArgIsExactlyMaxSize) {
  // PATH_MAX counts null terminator, so construct a path of size PATH_MAX-1.
  cpp::string s(PATH_MAX - 1, '/');
  for (size_t i = 1; i < s.size(); i += 2)
    s[i] = '.';

  ASSERT_STREQ(realpath_buffered(s), "/");
}

// Creates a test directory that has an absolute path with exactpy
// `desired_size` characters.
[[nodiscard]] bool create_abspath_with_size(TestDir &test_dir,
                                            size_t desired_size,
                                            cpp::string &out) {
  if (desired_size < test_dir.view().size() + PATH_SEP_SIZE) {
    tlog << "Test directory is already too long in create_abspath_with_size: "
         << test_dir.view().size() << "\n";
    return false;
  }
  size_t remaining_size = desired_size - test_dir.view().size() - PATH_SEP_SIZE;

  cpp::string relpath;
  relpath.reserve(remaining_size);

  while (remaining_size != 0) {
    if (!relpath.empty()) {
      relpath += '/';
      remaining_size -= PATH_SEP_SIZE;
    }

    size_t component_size = NAME_MAX;
    if (component_size > remaining_size)
      component_size = remaining_size;

    // If adding a component of this size would leave us in a state where
    // we only have enough space for a separator, shorten the component.
    if (remaining_size - component_size == PATH_SEP_SIZE)
      component_size -= 1;

    for (size_t i = 0; i < component_size; ++i)
      relpath += 'a';
    remaining_size -= component_size;

    if (test_dir.mkdir(relpath.c_str()))
      return false;
  }

  out = test_dir.abspath(relpath);

  if (out.size() != desired_size) {
    tlog << "Failed to create path of size=" << desired_size << "\n";
    return false;
  }
  return true;
}

TEST_F(LlvmLibcRealpathTest, OkIfResolvedPathIsExactlyMaxSize) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("OkIfResolvedPathIsExactlyMaxSize", test_dir));

  cpp::string path;
  ASSERT_TRUE(create_abspath_with_size(test_dir, PATH_MAX - 1, path));

  ASSERT_STREQ(realpath_buffered(path), path.c_str());
}

TEST_F(LlvmLibcRealpathTest, ErrorsWithNameTooLongIfPathArgExceedsMaxSize) {
  // PATH_MAX counts null terminator, so construct a path of size PATH_MAX.
  cpp::string s(PATH_MAX, '/');
  for (size_t i = 1; i < s.size(); i += 2)
    s[i] = '.';

  ASSERT_EQ(realpath_buffered(s), nullptr);
  ASSERT_ERRNO_EQ(ENAMETOOLONG);
}

TEST_F(LlvmLibcRealpathTest, RootResolvesToRoot) {
  ASSERT_STREQ(realpath_buffered("/"), "/");
}

TEST_F(LlvmLibcRealpathTest, RootDotDotTraversalStaysAtRoot) {
  ASSERT_STREQ(realpath_buffered("/.."), "/");
}

TEST_F(LlvmLibcRealpathTest, SimpleAbsolutePath) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("SimpleAbsolutePath", test_dir));

  ASSERT_THAT(test_dir.mkdir("a"), Succeeds());
  ASSERT_THAT(test_dir.mkdir("a/b"), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/b")),
               test_dir.abspath("a/b").c_str());
}

TEST_F(LlvmLibcRealpathTest, DotDotTraversesParent) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("DotDotTraversesParent", test_dir));

  ASSERT_THAT(test_dir.mkdir("a"), Succeeds());
  ASSERT_THAT(test_dir.mkdir("a/b"), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/b/..")),
               test_dir.abspath("a").c_str());
}

TEST_F(LlvmLibcRealpathTest, DotTraversalIsNop) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("DotTraversalIsNop", test_dir));

  ASSERT_THAT(test_dir.mkdir("a"), Succeeds());
  ASSERT_THAT(test_dir.mkdir("a/b"), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/b/./")),
               test_dir.abspath("a/b").c_str());
}

TEST_F(LlvmLibcRealpathTest, ConsecutiveSeparatorsIgnored) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("ConsecutiveSeparatorsIgnored", test_dir));

  ASSERT_THAT(test_dir.mkdir("a"), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a//..///a//")),
               test_dir.abspath("a").c_str());
}

TEST_F(LlvmLibcRealpathTest, AllocatesResultWhenBufferIsNull) {
  char *result = LIBC_NAMESPACE::realpath("/", nullptr);
  ASSERT_STREQ(result, "/");
  ::free(result);
}

TEST_F(LlvmLibcRealpathTest, ErrorsWithNotDirWhenFileIsInPath) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("ErrorsWithNotDirWhenFileIsInPath", test_dir));

  ASSERT_THAT(test_dir.touch("file"), Succeeds());

  ASSERT_EQ(realpath_buffered(test_dir.abspath("file/.")), nullptr);
  ASSERT_ERRNO_EQ(ENOTDIR);

  ASSERT_EQ(realpath_buffered(test_dir.abspath("file/")), nullptr);
  ASSERT_ERRNO_EQ(ENOTDIR);
}

TEST_F(LlvmLibcRealpathTest, FileAtEndOfPathIsOk) {
  TestDir test_dir;
  ASSERT_TRUE(create_test_dir("FileAtEndOfPathIsOk", test_dir));

  ASSERT_THAT(test_dir.mkdir("a"), Succeeds());
  ASSERT_THAT(test_dir.touch("a/file"), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/file")),
               test_dir.abspath("a/file").c_str());
}

TEST_F(LlvmLibcRealpathTest, ErrorsWithNoEntWhenComponentDoesNotExist) {
  TestDir test_dir;
  ASSERT_TRUE(
      create_test_dir("ErrorsWithNoEntWhenComponentDoesNotExist", test_dir));

  // A missing directory should give ENOENT.
  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/b")), nullptr);
  ASSERT_ERRNO_EQ(ENOENT);

  // Should fail if the final compnent doesn't exist.
  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a")), nullptr);
  ASSERT_ERRNO_EQ(ENOENT);
}

TEST_F(LlvmLibcRealpathTest, ErrorsWithNoAccesWhenDirectoryNotSearchable) {
  TestDir test_dir;
  ASSERT_TRUE(
      create_test_dir("ErrorsWithNoAccesWhenDirectoryNotSearchable", test_dir));

  ASSERT_THAT(test_dir.mkdir("a", /* mode= */ 0644), Succeeds());

  ASSERT_STREQ(realpath_buffered(test_dir.abspath("a/b")), nullptr);
  ASSERT_ERRNO_EQ(EACCES);
}
