// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/test/compatibility/v2/compatibility.h"

namespace f2fs {
std::string ConvertModeString(mode_t mode) {
  std::stringstream ss;
  ss << std::oct << mode;
  return ss.str();
}

std::string EscapedFilename(std::string_view filename) {
  std::set<char> special_character = {'`', '\"', '\\', '$'};

  std::string ret = "\"";

  for (size_t i = 0; i < filename.length(); ++i) {
    if (special_character.find(filename[i]) != special_character.end()) {
      ret.push_back('\\');
    }
    ret.push_back(filename[i]);
  }

  ret.push_back('\"');

  return ret;
}

bool LinuxTestFile::IsValid() {
  std::string result;
  linux_operator_->ExecuteWithAssert({"set +H;[ -e", EscapedFilename(filename_), "]; echo $?"},
                                     &result);
  return result == "0" || result == "0\n";
}

ssize_t LinuxTestFile::Write(const void* buf, size_t count) {
  const uint8_t* buf_c = static_cast<const uint8_t*>(buf);

  std::string hex_string;
  for (uint64_t i = 0; i < count; ++i) {
    char substring[10];
    sprintf(substring, "\\x%02x", buf_c[i]);
    hex_string.append(substring);

    if (i > 0 && i % 500 == 0) {
      linux_operator_->ExecuteWithAssert(
          {"echo", "-en", std::string("\"").append(hex_string).append("\""), ">>", filename_});
      hex_string.clear();
    }
  }

  linux_operator_->ExecuteWithAssert(
      {"echo", "-en", std::string("\"").append(hex_string).append("\""), ">>", filename_});
  linux_operator_->ExecuteWithAssert({"ls -al", filename_});

  return count;
}

int LinuxTestFile::Fchmod(mode_t mode) {
  std::string result;
  linux_operator_->ExecuteWithAssert({"chmod", ConvertModeString(mode), filename_}, &result);

  if (result.length() > 0) {
    return -1;
  }

  return 0;
}

int LinuxTestFile::Fstat(struct stat& file_stat) {
  std::string result;
  linux_operator_->ExecuteWithAssert({"stat", "-c", "\"%i %f %h %s %Z %Y %b\"", filename_},
                                     &result);

  std::vector<std::string> tokens;
  std::stringstream ss(result);
  std::string token;
  while (getline(ss, token, ' ')) {
    tokens.push_back(token);
  }

  file_stat.st_ino = std::stoul(tokens[0]);
  {
    std::stringstream ss;
    ss << std::hex << tokens[1];
    ss >> file_stat.st_mode;
  }
  file_stat.st_nlink = std::stoul(tokens[2]);
  file_stat.st_size = std::stol(tokens[3]);
  file_stat.st_ctime = std::stoul(tokens[4]);
  file_stat.st_mtime = std::stoul(tokens[5]);
  file_stat.st_blocks = std::stol(tokens[6]);

  return 0;
}

int LinuxTestFile::Ftruncate(off_t len) {
  std::string result;
  linux_operator_->ExecuteWithAssert({"truncate", "-s", std::to_string(len), filename_}, &result);
  return result.length() == 0 ? 0 : -1;
}

void LinuxTestFile::WritePattern(size_t block_count) {
  char buffer[kBlockSize];
  for (uint32_t i = 0; i < block_count; ++i) {
    memset(buffer, 0, sizeof(buffer));
    std::string pattern = std::to_string(i);
    pattern.copy(buffer, pattern.length());

    ASSERT_EQ(Write(buffer, sizeof(buffer)), static_cast<ssize_t>(sizeof(buffer)));
  }
}

void LinuxTestFile::VerifyPattern(size_t block_count) {
  for (uint32_t i = 0; i < block_count; ++i) {
    std::string result;
    linux_operator_->ExecuteWithAssert(
        {"od -An -j", std::to_string(i * kBlockSize), "-N",
         std::to_string(std::to_string(i).length()), "-c", filename_, "| tr -d ' \\n'"},
        &result);
    ASSERT_EQ(result, std::to_string(i));
  }
}

ssize_t FuchsiaTestFile::Read(void* buf, size_t count) {
  if (!vnode_->IsReg()) {
    return 0;
  }

  File* file = static_cast<File*>(vnode_.get());
  size_t ret = 0;

  if (file->Read(buf, count, offset_, &ret) != ZX_OK) {
    return 0;
  }

  offset_ += ret;

  return ret;
}

ssize_t FuchsiaTestFile::Write(const void* buf, size_t count) {
  if (!vnode_->IsReg()) {
    return 0;
  }

  File* file = static_cast<File*>(vnode_.get());
  size_t ret = 0;

  if (file->Write(buf, count, offset_, &ret) != ZX_OK) {
    return 0;
  }

  offset_ += ret;

  return ret;
}

int FuchsiaTestFile::Fstat(struct stat& file_stat) {
  fs::VnodeAttributes attr;
  if (zx_status_t status = vnode_->GetAttributes(&attr); status != ZX_OK) {
    return -EIO;
  }

  file_stat.st_ino = attr.inode;
  file_stat.st_mode = static_cast<mode_t>(attr.mode);
  file_stat.st_nlink = attr.link_count;
  file_stat.st_size = attr.content_size;
  file_stat.st_ctim.tv_sec = attr.creation_time / ZX_SEC(1);
  file_stat.st_ctim.tv_nsec = attr.creation_time % ZX_SEC(1);
  file_stat.st_mtim.tv_sec = attr.modification_time / ZX_SEC(1);
  file_stat.st_mtim.tv_nsec = attr.modification_time % ZX_SEC(1);
  file_stat.st_blocks = (vnode_->GetBlocks())
                        << vnode_->fs()->GetSuperblockInfo().GetLogSectorsPerBlock();

  return 0;
}

int FuchsiaTestFile::Ftruncate(off_t len) {
  if (!vnode_->IsReg()) {
    return -ENOTSUP;
  }

  File* file = static_cast<File*>(vnode_.get());
  if (zx_status_t status = file->Truncate(len); status != ZX_OK) {
    return -EIO;
  }

  return 0;
}

void FuchsiaTestFile::WritePattern(size_t block_count) {
  char buffer[kBlockSize];
  for (uint32_t i = 0; i < block_count; ++i) {
    std::memset(buffer, 0, kBlockSize);
    strcpy(buffer, std::to_string(i).c_str());
    ASSERT_EQ(Write(buffer, sizeof(buffer)), static_cast<ssize_t>(sizeof(buffer)));
  }
}

void FuchsiaTestFile::VerifyPattern(size_t block_count) {
  char buffer[kBlockSize];
  for (uint32_t i = 0; i < block_count; ++i) {
    ASSERT_EQ(Read(buffer, sizeof(buffer)), static_cast<ssize_t>(sizeof(buffer)));
    ASSERT_EQ(std::string(buffer), std::to_string(i));
  }
}

zx_status_t LinuxOperator::Execute(const std::vector<std::string>& argv, std::string* result) {
  return debian_guest_->Execute(argv, {}, zx::time::infinite(), result, nullptr);
}

void LinuxOperator::ExecuteWithAssert(const std::vector<std::string>& argv, std::string* result) {
  ASSERT_EQ(Execute(argv, result), ZX_OK);
}

std::string LinuxOperator::ConvertPath(std::string_view path) {
  // Convert only if |path| starts with |kLinuxPathPrefix|
  if (path.substr(0, kLinuxPathPrefix.length()) == kLinuxPathPrefix) {
    return std::string(mount_path_).append("/").append(path.substr(kLinuxPathPrefix.length()));
  }

  return std::string(path);
}

void LinuxOperator::Mkfs(std::string_view opt) {
  ExecuteWithAssert({"mkfs.f2fs", test_device_, "-f", opt.data()});
}

void LinuxOperator::Fsck() { ExecuteWithAssert({"fsck.f2fs", test_device_, "--dry-run"}); }

void LinuxOperator::Mount(std::string_view opt) {
  ExecuteWithAssert({"mkdir", "-p", mount_path_});
  ExecuteWithAssert({"mount", test_device_, mount_path_, opt.data()});
}

void LinuxOperator::Umount() { ExecuteWithAssert({"umount", mount_path_}); }

void LinuxOperator::Mkdir(std::string_view path, mode_t mode) {
  ExecuteWithAssert({"mkdir", "-m", ConvertModeString(mode), ConvertPath(path)});
}

int LinuxOperator::Rmdir(std::string_view path) {
  std::string result;
  ExecuteWithAssert({"rmdir", ConvertPath(path)}, &result);
  return (result.length() == 0) ? 0 : -1;
}

std::unique_ptr<TestFile> LinuxOperator::Open(std::string_view path, int flags, mode_t mode) {
  if (flags & O_CREAT) {
    if (flags & O_DIRECTORY) {
      Mkdir(path, mode);
    } else {
      ExecuteWithAssert({"set +H;touch", EscapedFilename(ConvertPath(path)), ";chmod ",
                         ConvertModeString(mode), EscapedFilename(ConvertPath(path))});
    }
  }

  return std::unique_ptr<TestFile>(new LinuxTestFile(ConvertPath(path), this));
}

void LinuxOperator::Rename(std::string_view oldpath, std::string_view newpath) {
  ExecuteWithAssert({"mv", ConvertPath(oldpath), ConvertPath(newpath)});
}

void FuchsiaOperator::Mkfs(MkfsOptions opt) {
  MkfsWorker mkfs(std::move(bc_), opt);
  auto ret = mkfs.DoMkfs();
  ASSERT_TRUE(ret.is_ok());
  bc_ = std::move(*ret);
}

void FuchsiaOperator::Fsck() {
  FsckWorker fsck(std::move(bc_), FsckOptions{.repair = false});
  ASSERT_EQ(fsck.Run(), ZX_OK);
  bc_ = fsck.Destroy();
}

void FuchsiaOperator::Mount(MountOptions opt) {
  auto vfs_or = Runner::CreateRunner(loop_.dispatcher());
  ASSERT_TRUE(vfs_or.is_ok());

  auto fs_or = F2fs::Create(nullptr, std::move(bc_), opt, (*vfs_or).get());
  ASSERT_TRUE(fs_or.is_ok());
  (*fs_or)->SetVfsForTests(std::move(*vfs_or));
  fs_ = std::move(*fs_or);

  ASSERT_EQ(VnodeF2fs::Vget(fs_.get(), fs_->RawSb().root_ino, &root_), ZX_OK);
  ASSERT_EQ(root_->Open(root_->ValidateOptions(fs::VnodeConnectionOptions()).value(), nullptr),
            ZX_OK);
}

void FuchsiaOperator::Umount() {
  ASSERT_EQ(root_->Close(), ZX_OK);
  root_.reset();

  fs_->SyncFs(true);
  fs_->PutSuper();

  auto vfs_or = fs_->TakeVfsForTests();
  ASSERT_TRUE(vfs_or.is_ok());

  auto bc_or = fs_->TakeBc();
  ASSERT_TRUE(bc_or.is_ok());
  bc_ = std::move(*bc_or);

  (*vfs_or).reset();
}

void FuchsiaOperator::Mkdir(std::string_view path, mode_t mode) {
  auto new_dir = Open(path, O_CREAT | O_EXCL, S_IFDIR | mode);
  ASSERT_TRUE(new_dir->IsValid());
}

int FuchsiaOperator::Rmdir(std::string_view path) {
  auto ret = GetLastDirVnodeAndFileName(path);
  if (ret.is_error()) {
    return -1;
  }
  auto [parent_vnode, child_name] = ret.value();

  if (zx_status_t status = fs_->vfs()->Unlink(parent_vnode, child_name, true); status != ZX_OK) {
    return -1;
  }
  return 0;
}

std::unique_ptr<TestFile> FuchsiaOperator::Open(std::string_view path, int flags, mode_t mode) {
  auto result = fs_->vfs()->Open(root_, path, ConvertFlag(flags), fs::Rights::ReadWrite(), mode);
  if (result.is_error()) {
    return std::unique_ptr<TestFile>(new FuchsiaTestFile(nullptr));
  }

  fbl::RefPtr<VnodeF2fs> vnode = fbl::RefPtr<VnodeF2fs>::Downcast(result.ok().vnode);

  std::unique_ptr<TestFile> ret = std::make_unique<FuchsiaTestFile>(std::move(vnode));

  return ret;
}

void FuchsiaOperator::Rename(std::string_view oldpath, std::string_view newpath) {
  auto result = GetLastDirVnodeAndFileName(oldpath);
  ASSERT_TRUE(result.is_ok());
  auto [oldparent_vn, oldchild_name] = result.value();

  result = GetLastDirVnodeAndFileName(newpath);
  ASSERT_TRUE(result.is_ok());
  auto [newparent_vn, newchild_name] = result.value();

  ASSERT_EQ(oldparent_vn->Rename(newparent_vn, oldchild_name, newchild_name, false, false), ZX_OK);
}

zx::result<std::pair<fbl::RefPtr<fs::Vnode>, std::string>>
FuchsiaOperator::GetLastDirVnodeAndFileName(std::string_view absolute_path) {
  std::filesystem::path path(absolute_path);
  if (!path.has_root_directory() || !path.has_filename()) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  fbl::RefPtr<fs::Vnode> vnode = root_;

  for (auto token : path.parent_path().relative_path()) {
    if (auto ret = vnode->Lookup(token.string(), &vnode); ret != ZX_OK) {
      return zx::error(ret);
    }
  }
  return zx::ok(make_pair(std::move(vnode), path.filename().string()));
}

fs::VnodeConnectionOptions ConvertFlag(int flags) {
  fs::VnodeConnectionOptions options;

  // TODO: O_PATH, O_DIRECT, O_TRUNC, O_APPEND
  switch (flags & O_ACCMODE) {
    case O_RDONLY:
      options.rights.read = true;
      break;
    case O_WRONLY:
      options.rights.write = true;
      break;
    case O_RDWR:
      options.rights.read = true;
      options.rights.write = true;
      break;
    default:
      break;
  }

  if (flags & O_CREAT) {
    options.flags.create = true;
  }
  if (flags & O_EXCL) {
    options.flags.fail_if_exists = true;
  }

  return options;
}

}  // namespace f2fs
