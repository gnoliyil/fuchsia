#!/usr/bin/env python3.8
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from __future__ import print_function

import argparse
import functools
import json
import logging
import os
import platform
import stat
import subprocess
import sys
import tempfile
import time
import typing

from typing import List

try:
  global CGPT_BIN
  import paths
  CGPT_BIN = os.path.join(paths.PREBUILT_DIR, 'tools/cgpt',
                                paths.PREBUILT_PLATFORM, 'cgpt')
except ImportError as e:
  # If we're being run via `fx`, we take paths for cgpt and the Fuchsia images from
  # the `import paths` above. Otherwise, we expect the paths to be provided via
  # command-line arguments. The command-line arguments also override the autodetected
  # paths if they're both present.
  pass

if sys.hexversion < 0x030700F0:
  logging.critical(
      'This script requires Python >= 3.7 to run (you have %s), please upgrade!'
      % (platform.python_version()))
  sys.exit(1)

# The GUIDs are from zircon/system/public/zircon/hw/gpt.h
WORKSTATION_INSTALLER_GPT_GUID = '4dce98ce-e77e-45c1-a863-caf92f1330c1'
ZIRCON_R_GPT_GUID = 'a0e5cf57-2def-46be-a80c-a2067c37cd49'
VBMETA_R_GPT_GUID = '6a2460c3-cd11-4e8b-80a8-12cce268ed0a'
ABR_META_GPT_GUID = '1d75395d-f2c6-476b-a8b7-45cc1c97b476'

def make_unique_name(name, type):
  return f'{name}_{type}'

class ManifestImage:
  """ Represents an entry in the 'images.json' manifest that will be installed to disk. """
  def __init__(self, name: str, guids: List[str], type: str, dest_name: str = None):
    """
    Args:
      name: 'name' of the partition in the image manifest.
      guids: List of GPT GUIDs that this partition should be written to the disk with.
      type: 'type' of the partition in the image manifest.
    """
    self.name = name
    self.guids = guids
    self.type = type
    if dest_name:
        self.dest_name = dest_name
    else:
        self.dest_name = name

  def unique_name(self):
    return make_unique_name(self.name, self.type)


# This is the list of Fuchsia build images we write to the final image,
# and the partition types they will have (passed to cgpt)
IMAGES_RECOVERY_INSTALLER = [
    # The recovery image for chromebook-x64.
    ManifestImage('recovery-installer', ['kernel'], 'zbi.signed', 'zircon-r'),

    # The recovery image and a bootloader for x64.
    ManifestImage('recovery-installer', [ZIRCON_R_GPT_GUID], 'zbi', 'zircon-r'),
    ManifestImage('recovery-installer', [VBMETA_R_GPT_GUID], 'vbmeta', 'vbmeta_r'),
    ManifestImage('fuchsia.esp', ['efi'], 'blk'),

    # Standard x64 partitions
    # This is the EFI system partition that will be installed to the target.
    ManifestImage('fuchsia.esp', [WORKSTATION_INSTALLER_GPT_GUID], 'blk'),
    ManifestImage('zircon-a', [WORKSTATION_INSTALLER_GPT_GUID], 'zbi'),
    ManifestImage('zircon-r', [WORKSTATION_INSTALLER_GPT_GUID], 'zbi'),

    # ChromeOS partitions
    # The zircon-r.signed partition is used as both zedboot on the installation
    # disk and also the installed zircon-r partition.
    ManifestImage('zircon-r.signed', [WORKSTATION_INSTALLER_GPT_GUID], 'zbi.signed'),
    ManifestImage('zircon-a.signed', [WORKSTATION_INSTALLER_GPT_GUID], 'zbi.signed'),

    # Common partitions - installed everywhere.
    ManifestImage('storage-sparse', [WORKSTATION_INSTALLER_GPT_GUID], 'blk'),
]

# This is the list of images for running recovery-eng, which offers userspace
# fastboot.
IMAGES_RECOVERY_FASTBOOT = [
    # The recovery-eng image for chromebook-x64.
    ManifestImage('recovery-eng', ['kernel'], 'zbi.signed', 'zircon-r'),

    # The recovery-eng image and a bootloader for x64.
    ManifestImage('recovery-eng', [ZIRCON_R_GPT_GUID], 'zbi', 'zircon-r'),
    ManifestImage('fuchsia.esp', ['efi'], 'blk'),
]

def ParseSize(size):
  """Parse a size.

  Args:
    size: '<number><suffix>', where suffix is 'K', 'M', or 'G'.

  Returns:
    A size in bytes equivalent to the human-readable size given.
  Raises:
    A ValueError if <suffix> is unrecognised or <number> is not a base-10
    number.
  """
  units = ['K', 'M', 'G']
  if size.isdigit():
    return int(size)
  else:
    unit = size[-1].upper()
    size_bytes = int(size[:-1]) * 1024

  if unit not in units:
    raise ValueError('unrecognised unit suffix "{}" for size {}'.format(
        unit, size))

  while units.pop(0) != unit:
    size_bytes *= 1024
  return size_bytes


def PrettySize(size):
  """Returns a size in bytes as a human-readable string."""
  units = 'BKMGT'

  unit = 0
  # By the time we get to 3072, the error caused by
  # shifting units is <2%, so we don't care.
  while size > 3072 and unit < len(units) - 1:
    size /= 1024
    unit += 1

  return '{:1.1f}{}'.format(size, units[unit])


class Partition:
  """Represents a single partition to be written to the disk.

  Attributes:
    label: label of the partition on the output image.
    path: path of the file that is going to be written to the partition on
      the host.
    real_size: size of the file on the host, in bytes.
    size: size of the partition, in bytes. This may not match real_size due
      to sector size alignment or EFI partition rules.
    type: type of the partition, passed to `cgpt`.
  """
  FAT32_MIN_SIZE = (63 * 1024 * 1024)

  def __init__(self, path, part_type, label, size: typing.Optional[int] = None):
    self.path = path
    self.type = part_type
    self.label = label

    # Calculate sector-aligned size of this file.
    if path:
      stat_result = os.stat(path)
      if not stat.S_ISREG(stat_result.st_mode):
        raise ValueError('{} is not a regular file.'.format(path))
      rounded_size = stat_result.st_size
      size = stat_result.st_size
    elif size:
      rounded_size = size
    else:
      raise ValueError('Expected a source file or size')
    if rounded_size % Image.SECTOR_SIZE != 0:
      rounded_size += Image.SECTOR_SIZE
      rounded_size -= rounded_size % Image.SECTOR_SIZE
    if self.type == 'efi':
      # Gigaboot won't be able to load zedboot.bin from an EFI partition that's
      # too small, so we ensure the partition is at least big enough for it to
      # work.
      rounded_size = max(Partition.FAT32_MIN_SIZE, rounded_size)
    self.real_size = size
    self.size = rounded_size


class Image:
  """Represents a single disk image to be written.

    Attributes:
        filename: output filename of the image.
        is_usb: True if writing to a USB, False if creating an image on the
          host.
        file: filehandle to the output image. Held open while we work to prevent
          auto-mounting of USB.
        block_size: number of bytes to write at a time to the disk.
        file_size: total size of the image, in bytes
        partitions: list of |Partition| objects to write to disk.
  """
  SECTOR_SIZE = 512
  GPT_SECTORS = 2048
  CROS_RESERVED_SECTORS = 2048

  def __init__(self, filename, is_usb, block_size):
    self.filename = filename
    self.is_usb = is_usb
    self.file = open(filename, mode='wb')
    self.block_size = block_size

    # Allocate space for the primary and backup GPTs
    self.file_size = 2 * Image.GPT_SECTORS * Image.SECTOR_SIZE
    self.partitions = []

    # Set up the ChromeOS reserved partition.
    reserved_part = Partition(None, 'reserved', 'reserved',
                              self.CROS_RESERVED_SECTORS * self.SECTOR_SIZE)
    self.AddPartition(reserved_part)

  def _Cgpt(self, args):
    args = [CGPT_BIN] + args
    return subprocess.run(args, capture_output=True)

  def _CgptAdd(self, part, offset):
    """Add a partition to the GPT represnted by thsis |Image|.

    Args:
      part: partition to add
      offset: offset to add partition at. Must be a multiple of SECTOR_SIZE.
    Returns:
      True if add succeded, False if it failed.
    """
    if offset % Image.SECTOR_SIZE != 0:
      raise ValueError('Offset must be a multiple of SECTOR_SIZE!')
    if part.size % Image.SECTOR_SIZE != 0:
      raise ValueError('Size must be a multiple of SECTOR_SIZE!')
    size = part.size // Image.SECTOR_SIZE
    offset //= Image.SECTOR_SIZE
    cgpt_args = ['add', '-s', str(size), '-t', part.type, '-b', str(offset),
                 '-l', part.label, self.filename]

    if part.type == 'kernel':
      # Mark CrOS kernel partition as bootable.
      # We assume that the disk will only have 1 kernel partition.
      # -T 1 = Bootloader will try to boot this partition once.
      # -S 0 = This partition has been successfully booted.
      # -P 2 = Partition priority is 2. Partition with the highest priority gets
      # booted.
      cgpt_args += ['-T', '1', '-S', '1', '-P', '2']

    ret = self._Cgpt(cgpt_args)
    if ret.returncode != 0:
      logging.critical('\n'
            '======= CGPT ADD FAILED! =======\n'
            'Maybe your disk is too small?\n')
      logging.error(ret.stdout)
      logging.error(ret.stderr)
      return False
    return True

  def AddPartition(self, partition):
    """Add a partition to the outputted disk image.

    This function does not write any data - call Finalise() to write the
    disk image once all partitions have been added.

    Args:
      partition: partition to add
    """
    self.partitions.append(partition)
    self.file_size += partition.size

  def WritePart(self, part, offset):
    """Writes data to a partition on the output device.

    Args:
      part: partiton to write
      offset: offset in bytes to write to
    """
    if part.path is None:
      return
    self.file.seek(offset, 0)

    written = 0
    logging.info(
        '   Writing image {} to partition {}... '.format(
            part.path.split('/')[-1], part.label)
        )
    with open(part.path, 'rb') as fh:
      start = time.perf_counter()
      for block in iter(functools.partial(fh.read, self.block_size), b''):
        written += len(block)
        self.file.write(block)
      # flush and fsync to get accurate timing results
      self.file.flush()
      os.fsync(self.file.fileno())
      finish = time.perf_counter()
    per_second = written / (finish - start)
    logging.info('     Wrote {} in {:1.2f}s, {}/s'.format(
        PrettySize(written), finish - start, PrettySize(per_second)))

  def Finalise(self):
    """Write all the partitions this image represents to disk/file."""
    if not self.is_usb:
      # first, make sure the file is big enough.
      logging.info('Create image of size={} bytes'.format(self.file_size))
      self.file.truncate(self.file_size)
      self.file.flush()
      os.fsync(self.file.fileno())

    logging.info('Creating new GPT partition table...')
    self._Cgpt(['create', self.filename])
    self._Cgpt(['boot', '-p', self.filename])
    logging.info('Done.')

    logging.info('Creating and writing partitions...')
    current_offset = Image.SECTOR_SIZE * Image.GPT_SECTORS
    for part in self.partitions:
      if not self._CgptAdd(part, current_offset):
        logging.warning('Write failed, aborting.')
        self.file.close()
        return
      self.WritePart(part, current_offset)
      current_offset += part.size
    logging.info('Done.')
    self.file.close()

def GetPartitions(build_dir, images_file, target_images):
  """Get all partitions to be written to the output image.

  The list of partitions is currently determined by the IMAGES dict
  at the top of this file.

  Args:
    build_dir: path to the build directory containing images.
    images_file: path to images.json. If None, will generate from build_dir

  Returns:
    a list of |Partition| objects to be written to the disk.
  """
  use_signed_images = False

  images = {}
  if images_file == '':
    images_file = os.path.join(build_dir, 'images.json')
  try:
    with open(images_file) as f:
      images_list = json.load(f)
      for image in images_list:
        images[make_unique_name(image['name'], image['type'])] = image
        if image['type'] == 'zbi.signed':
          use_signed_images = True
  except IOError as err:
    logging.critical('Failed to find image manifest. Have you run `fx build`?', exc_info=err)
    return []

  ret = []
  is_bootable = False
  for image in target_images:
    if image.unique_name() not in images:
      logging.debug("Skipping image that wasn't built: {}".format(image.unique_name()))
      continue

    if use_signed_images and image.type == 'zbi':
      logging.debug("Skipping unsigned image: {}".format(image.unique_name()))
      continue

    for part_type in image.guids:
      full_path = os.path.join(build_dir, images[image.unique_name()]['path'])
      ret.append(Partition(full_path, part_type, image.dest_name))
      # Assume that any non-installer partition is a bootable partition.
      if part_type != WORKSTATION_INSTALLER_GPT_GUID:
        is_bootable = True

  if not is_bootable:
    logging.critical('ERROR: mkinstaller would generate an unbootable image.' +
          'Are you building for a supported platform?')
    return []

  return ret


def GetUsbDisks():
  """Get a list of all USB disks on the system.

  Returns:
    A list where each entry is of the format '/path/to/disk - <disk name>'
  """
  res = subprocess.run(['fx', 'list-usb-disks'], capture_output=True)
  res.check_returncode()

  disks = [d for d in res.stdout.decode('utf-8').split('\n') if d]
  return disks


def IsUsbDisk(path):
  """Is the given path a USB disk?

  Args:
    path: a path that may represent a USB disk. Does not have to exist.

  Returns:
    True if the path represents a USB disk, False otherwise.
  """
  return path in map(lambda a: a.split()[0], GetUsbDisks())


def UnmountDisk(path):
  """Unmount the given USB disk from the system."""
  system = platform.system()
  if system == 'Darwin':
    subprocess.run(['diskutil', 'quiet', 'unmountDisk', path])


def EjectDisk(path):
  """Eject the given USB disk from the system."""
  system = platform.system()
  if system == 'Linux':
    subprocess.run(['eject', path])
  elif system == 'Darwin':
    subprocess.run(['diskutil', 'eject', path])
  logging.info('Ejected USB disk')


def Main(args):
  path = args.FILE
  if args.create:
    if not args.force and os.path.exists(path):
      logging.critical(
          'File {} already exists, not creating an image. Use --force if you want to proceed.'
          .format(path))
      return 1
  else:
    if not os.path.exists(path):
      logging.critical(
          ('Path {} does not exist, use --create to create a disk image.\n'
           'Detected USB devices:\n'
           '{}').format(path, '\n'.join(GetUsbDisks())))
      return 1
    if not IsUsbDisk(path):
      logging.critical(
          ('Path {} is not a USB device. Use -f to force.\n'
           'Detected USB devices:\n'
           '{}').format(path, '\n'.join(GetUsbDisks())))
      return 1

    if not os.access(path, os.W_OK) and sys.stdin.isatty():
      logging.warning('Changing ownership of {} to {}'.format(path,
                                                    os.environ.get('USER')))
      subprocess.run(
          ['sudo', 'chown', os.environ.get('USER'), path],
          stdin=sys.stdin,
          stdout=sys.stdout,
          stderr=sys.stderr)
    elif not os.access(path, os.W_OK):
      logging.critical('Cannot write to {}. Please check file permissions!'.format(path))
      return 1

    UnmountDisk(path)

  build_dir = args.build_dir
  if build_dir == '':
    build_dir = paths.FUCHSIA_BUILD_DIR

  target_images = IMAGES_RECOVERY_FASTBOOT if args.recovery_fastboot else IMAGES_RECOVERY_INSTALLER
  parts = GetPartitions(build_dir, args.images, target_images)
  if not parts:
    return 1

  # Add a abr metadata partition
  with tempfile.TemporaryDirectory() as temp_dir:
    # A pre-generated abr metadata that can only boots zircon-r
    abr_data_file = os.path.join(temp_dir, 'abr_data')
    with open(abr_data_file, 'wb') as abr_data:
      abr_data.write(
        (
          b'\x00\x41\x42\x30\x02\x01\x00\x00\x0f\x00\x00\x00\x0e\x00\x00\x00\x00'
          b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x6b\xa3\x22\x12'
        ).ljust(1024 * 1024)  # Make a 1MB partition.
      )

    #TODO(b/268532862): Use new GUID once switched to gigaboot++.
    parts.append(
      Partition(str(abr_data_file), ABR_META_GPT_GUID, 'durable_boot'))

    output = Image(args.FILE, not args.create, ParseSize(args.block_size))
    for p in parts:
      output.AddPartition(p)

    output.Finalise()
  if not args.create:
    EjectDisk(path)
  return 0


if __name__ == '__main__':
  parser = argparse.ArgumentParser(
      description='Create a Fuchsia installer image.', prog='fx mkinstaller')
  parser.add_argument(
      '-c',
      '--create',
      action='store_true',
      help='Create a disk image instead of writing to an existing disk.')
  parser.add_argument(
      '-f',
      '--force',
      action='store_true',
      help='Force writing to an image that already exists or a disk that might not be a USB.'
  )
  parser.add_argument(
      '-b',
      '--block-size',
      type=str,
      default='2M',
      help='Block size (optionally suffixed by K, M, G) to write. Default is 2M'
  )
  parser.add_argument(
      '-v',
      '--verbose',
      action='store_true',
      help='Be verbose while creating disk images.'
  )
  parser.add_argument(
      '--cgpt-path',
      type=str,
      default='',
      help='Path to cgpt in the Fuchsia tree. The script will try and guess if no path is provided.'
  )
  parser.add_argument(
      '--images',
      type=str,
      default='',
      help='Path to images.json in the Fuchsia tree. Default to build_dir/images.json.'
  )
  parser.add_argument(
      '--build-dir',
      type=str,
      default='',
      help='Path to the Fuchsia build directory. The script will try and guess if no path is provided.'
  )
  parser.add_argument(
      '--new-installer',
      action='store_true',
      help='DEPRECATED. Has no effect.'
  )
  parser.add_argument(
      '--recovery-fastboot',
      action='store_true',
      help='Create a bootable recovery-eng image with userspace fastboot.')
  parser.add_argument('FILE', help='Path to USB device or installer image')
  argv = parser.parse_args()
  level = logging.WARNING
  if argv.verbose:
    level = logging.DEBUG
  if argv.cgpt_path:
    CGPT_BIN = argv.cgpt_path
  logging.basicConfig(format='mkinstaller: %(levelname)s: %(message)s', level=level)
  sys.exit(Main(argv))
