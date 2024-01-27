// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"go.fuchsia.dev/fuchsia/tools/emulator"
	"go.fuchsia.dev/fuchsia/tools/emulator/emulatortest"
)

func TestKtraceWorksWhenEnabled(t *testing.T) {
	exDir := execDir(t)
	distro := emulatortest.UnpackFrom(t, filepath.Join(exDir, "test_data"), emulator.DistributionParams{
		Emulator: emulator.Qemu,
	})
	arch := distro.TargetCPU()
	device := emulator.DefaultVirtualDevice(string(arch))
	device.Initrd = "zircon-r" // zedboot zbi.
	device.KernelArgs = append(device.KernelArgs, "kernel.enable-debugging-syscalls=true")

	stdout, stderr := distro.RunNonInteractive(
		"/boot/bin/ktrace start 0xff",
		filepath.Join(exDir, "test_data", "tools", "extract-logs"),
		filepath.Join(exDir, "test_data", "tools", "zbi"),
		device,
	)

	ensureDoesNotContain(t, stdout, "ZX_ERR_NOT_SUPPORTED")
	ensureDoesNotContain(t, stderr, "ZX_ERR_NOT_SUPPORTED")
}

func ensureDoesNotContain(t *testing.T, output, lookFor string) {
	if strings.Contains(output, lookFor) {
		t.Fatalf("output contains '%s'", lookFor)
	}
}

func execDir(t *testing.T) string {
	ex, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	return filepath.Dir(ex)
}
