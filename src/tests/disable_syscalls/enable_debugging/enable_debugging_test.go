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

func TestEnableDebuggingSyscalls(t *testing.T) {
	exDir := execDir(t)
	distro := emulatortest.UnpackFrom(t, filepath.Join(exDir, "test_data"), emulator.DistributionParams{
		Emulator: emulator.Qemu,
	})
	arch := distro.TargetCPU()
	device := emulator.DefaultVirtualDevice(string(arch))
	device.Initrd = "zircon-r" // zedboot zbi.
	device.KernelArgs = append(device.KernelArgs, "kernel.enable-debugging-syscalls=true ", "kernel.enable-serial-syscalls=true")

	stdout, stderr := distro.RunNonInteractive(
		"/boot/bin/syscall-check",
		filepath.Join(exDir, "test_data", "tools", "extract-logs"),
		filepath.Join(exDir, "test_data", "tools", "zbi"),
		device,
	)

	ensureContains(t, stdout, "zx_debug_read: enabled")
	ensureContains(t, stdout, "zx_debug_send_command: enabled")
	ensureContains(t, stdout, "zx_debug_write: enabled")
	ensureContains(t, stdout, "zx_ktrace_control: enabled")
	ensureContains(t, stdout, "zx_ktrace_read: enabled")
	ensureContains(t, stdout, "zx_ktrace_write: enabled")
	ensureContains(t, stdout, "zx_mtrace_control: enabled")
	ensureContains(t, stdout, "zx_process_write_memory: enabled")
	ensureContains(t, stdout, "zx_system_mexec: enabled")
	ensureContains(t, stdout, "zx_system_mexec_payload_get: enabled")
	ensureContains(t, stdout, "zx_thread_write_state: enabled")
	if stderr != "" {
		t.Fatal(stderr)
	}
}

func execDir(t *testing.T) string {
	ex, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	return filepath.Dir(ex)
}

func ensureContains(t *testing.T, output string, lookFor string) {
	if !strings.Contains(output, lookFor) {
		t.Fatalf("output did not contain '%s'", lookFor)
	}
}
