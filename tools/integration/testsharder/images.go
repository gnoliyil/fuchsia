// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testsharder

import (
	"errors"
	"os"
	"path/filepath"

	"go.fuchsia.dev/fuchsia/tools/build"
)

// AddImageDeps selects and adds the subset of images needed by a shard to
// that shard's list of dependencies.
func AddImageDeps(s *Shard, buildDir string, images []build.Image, pave bool) error {
	// Host-test only shards do not require any image deps because they are not running
	// against a Fuchsia target.
	if s.Env.Dimensions.DeviceType == "" {
		return nil
	}
	imageDeps := []string{"images.json"}
	// GCE test shards do not require any image deps as the build creates a
	// compute image with all the deps baked in.
	if s.Env.Dimensions.DeviceType == "GCE" {
		s.AddDeps(imageDeps)
		return nil
	}
	for _, image := range images {
		if isUsedForTesting(s, image, pave) {
			if _, err := os.Stat(filepath.Join(buildDir, image.Path)); err != nil {
				if !errors.Is(err, os.ErrNotExist) {
					return err
				}
			} else {
				imageDeps = append(imageDeps, image.Path)
			}
		}
	}
	s.AddDeps(imageDeps)
	return nil
}

func isUsedForTesting(s *Shard, image build.Image, pave bool) bool {
	// If image overrides have been specified, then by convention we only wish
	// to select among the images that could be overridden.
	overrides := s.Env.ImageOverrides
	if !overrides.IsEmpty() {
		if image.Label == overrides.ZBI || image.Label == overrides.VBMeta || image.Label == overrides.QEMUKernel || image.Label == overrides.EFIDisk {
			return true
		}

		// Emulators always need a kernel or a UEFI filesystem/disk image.
		if s.Env.TargetsEmulator() && image.Name == "qemu-kernel" && overrides.QEMUKernel == "" && overrides.EFIDisk == "" {
			return true
		}

		// TODO(fxbug.dev/47531): Remove zedboot images once we switch to flashing.
		return !s.Env.TargetsEmulator() && len(image.PaveZedbootArgs) != 0
	}

	if s.Env.TargetsEmulator() {
		// This provisions the images used by EMU targets in botanist:
		// https://cs.opensource.google/fuchsia/fuchsia/+/master:tools/botanist/targets/qemu.go?q=zbi_zircon
		return image.Name == "qemu-kernel" || image.Name == "storage-full" || image.Name == "zircon-a"
	}
	if isFlashingDep(image) {
		return true
	}
	// TODO(fxbug.dev/47531): Remove zedboot/paving images once we switch to flashing.
	return ((pave && len(image.PaveArgs) != 0) ||
		(!pave && len(image.NetbootArgs) != 0) ||
		(len(image.PaveZedbootArgs) != 0) ||
		(pave && len(image.FastbootFlashArgs) != 0) ||
		(!pave && len(image.FastbootBootArgs) != 0))
}

func isFlashingDep(image build.Image) bool {
	return image.Name == "flash-script" || image.Name == "fastboot" || image.Name == "fastboot-boot-script"
}
