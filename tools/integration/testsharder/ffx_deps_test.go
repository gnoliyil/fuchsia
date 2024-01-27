// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testsharder

import (
	"os"
	"path/filepath"
	"sort"
	"testing"

	"github.com/google/go-cmp/cmp"

	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/lib/ffxutil"
	"go.fuchsia.dev/fuchsia/tools/lib/jsonutil"
)

func TestAddFFXDeps(t *testing.T) {
	baseDeps := []string{
		"sdk/manifest/core",
	}
	testCases := []struct {
		name       string
		targetCPU  string
		deviceType string
		want       []string
	}{
		{
			name:       "QEMU x64 deps",
			targetCPU:  "x64",
			deviceType: "QEMU",
			want:       baseDeps,
		},
		{
			name:       "NUC bootloader boot deps",
			targetCPU:  "x64",
			deviceType: "NUC",
			want:       []string{"zircon-a.zbi", "zircon-a.vbmeta"},
		},
		{
			name:       "AEMU x64 deps",
			targetCPU:  "x64",
			deviceType: "AEMU",
			want:       baseDeps,
		},
		{
			name:       "QEMU arm64 deps",
			targetCPU:  "arm64",
			deviceType: "QEMU",
			want:       baseDeps,
		},
	}
	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			buildDir := t.TempDir()
			manifestPath := filepath.Join(buildDir, ffxutil.SDKManifestPath)
			if err := os.MkdirAll(filepath.Dir(manifestPath), os.ModePerm); err != nil {
				t.Fatalf("failed to mkdirAll %s: %s", filepath.Dir(manifestPath), err)
			}
			if err := jsonutil.WriteToFile(manifestPath, ffxutil.SDKManifest{Atoms: []ffxutil.Atom{}}); err != nil {
				t.Fatalf("failed to write manifest at %s: %s", manifestPath, err)
			}
			s := &Shard{
				Env: build.Environment{
					Dimensions: build.DimensionSet{
						"device_type": tc.deviceType,
					},
				},
				Tests: []Test{{Test: build.Test{CPU: tc.targetCPU}}},
			}
			if err := AddFFXDeps(s, buildDir, []build.Image{
				{Name: "zircon-a", Path: "zircon-a.zbi", Type: "zbi"},
				{Name: "zircon-a", Path: "zircon-a.vbmeta", Type: "vbmeta"},
				{Name: "fuchsia", Path: "fuchsia.zbi", Type: "zbi"},
			}, false); err != nil {
				t.Errorf("failed to add ffx deps: %s", err)
			}
			sort.Strings(tc.want)
			if diff := cmp.Diff(tc.want, s.Deps); diff != "" {
				t.Errorf("AddFFXDeps(%v, %s) failed: (-want +got): \n%s", s, buildDir, diff)
			}
		})
	}
}
