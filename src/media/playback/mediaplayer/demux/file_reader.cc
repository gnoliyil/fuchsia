// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/demux/file_reader.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/syslog/cpp/macros.h>
#include <unistd.h>

#include "src/lib/files/file_descriptor.h"
#include "src/lib/fsl/io/fd.h"

namespace media_player {

// static
std::shared_ptr<FileReader> FileReader::Create(zx::channel file_channel) {
  return std::make_shared<FileReader>(fsl::OpenChannelAsFileDescriptor(std::move(file_channel)));
}

FileReader::FileReader(fbl::unique_fd fd)
    : dispatcher_(async_get_default_dispatcher()), fd_(std::move(fd)) {
  FX_DCHECK(dispatcher_);

  status_ = fd_.is_valid() ? ZX_OK : ZX_ERR_NOT_FOUND;

  if (status_ == ZX_OK) {
    off_t seek_result = lseek(fd_.get(), 0, SEEK_END);
    if (seek_result >= 0) {
      size_ = static_cast<uint64_t>(seek_result);
    } else {
      size_ = kUnknownSize;
      // TODO(dalesat): More specific error code.
      status_ = ZX_ERR_INTERNAL;
    }
  }
}

FileReader::~FileReader() {}

void FileReader::Describe(DescribeCallback callback) {
  async::PostTask(dispatcher_,
                  [this, callback = std::move(callback)]() { callback(status_, size_, true); });
}

void FileReader::ReadAt(size_t position, uint8_t* buffer, size_t bytes_to_read,
                        ReadAtCallback callback) {
  FX_DCHECK(position < size_);

  if (status_ != ZX_OK) {
    callback(status_, 0);
    return;
  }

  off_t seek_result = lseek(fd_.get(), position, SEEK_SET);
  if (seek_result < 0) {
    FX_LOGS(ERROR) << "seek failed, result " << seek_result << " errno " << errno;
    // TODO(dalesat): More specific error code.
    status_ = ZX_ERR_INTERNAL;
    callback(status_, 0);
    return;
  }

  ssize_t result =
      fxl::ReadFileDescriptor(fd_.get(), reinterpret_cast<char*>(buffer), bytes_to_read);
  if (result < 0) {
    // TODO(dalesat): More specific error code.
    status_ = ZX_ERR_INTERNAL;
    callback(status_, 0);
    return;
  }

  async::PostTask(dispatcher_,
                  [callback = std::move(callback), result]() { callback(ZX_OK, result); });
}

}  // namespace media_player
