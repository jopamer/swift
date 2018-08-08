//===--- FileSystem.h - Extra helpers for manipulating files ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_FILESYSTEM_H
#define SWIFT_BASIC_FILESYSTEM_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include <system_error>

namespace llvm {
  class raw_pwrite_stream;
  class Twine;
}

namespace clang {
  namespace vfs {
    class FileSystem;
  }
}

namespace swift {
  /// Invokes \p action with a raw_ostream that refers to a temporary file,
  /// which is then renamed into place as \p outputPath when the action
  /// completes.
  ///
  /// If a temporary file cannot be created for whatever reason, \p action will
  /// be invoked with a stream directly opened at \p outputPath. Otherwise, if
  /// there is already a file at \p outputPath, it will not be overwritten if
  /// the new contents are identical.
  ///
  /// If the process is interrupted with a signal, any temporary file will be
  /// removed.
  ///
  /// As a special case, an output path of "-" is treated as referring to
  /// stdout.
  std::error_code atomicallyWritingToFile(
      llvm::StringRef outputPath,
      llvm::function_ref<void(llvm::raw_pwrite_stream &)> action);

  /// Moves a file from \p source to \p destination, unless there is already
  /// a file at \p destination that contains the same data as \p source.
  ///
  /// In the latter case, the file at \p source is deleted. If an error occurs,
  /// the file at \p source will still be present at \p source.
  std::error_code moveFileIfDifferent(const llvm::Twine &source,
                                      const llvm::Twine &destination);

  namespace vfs {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
    getFileOrSTDIN(clang::vfs::FileSystem &FS,
                   const llvm::Twine &Name, int64_t FileSize = -1,
                   bool RequiresNullTerminator = true, bool IsVolatile = false);
  } // end namespace vfs

} // end namespace swift

#endif // SWIFT_BASIC_FILESYSTEM_H
