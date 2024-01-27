// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package recovery

import (
	"flag"
	"os"
	"path/filepath"
	"time"

	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/cli"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/util"
)

type config struct {
	archiveConfig    *cli.ArchiveConfig
	deviceConfig     *cli.DeviceConfig
	installerConfig  *cli.InstallerConfig
	buildConfig      *cli.BuildConfig
	packagesPath     string
	paveTimeout      time.Duration
	cycleCount       int
	cycleTimeout     time.Duration
	beforeInitScript string
	afterInitScript  string
	afterTestScript  string
	useFlash         bool
}

func newConfig(fs *flag.FlagSet) (*config, error) {
	testDataPath := filepath.Join(filepath.Dir(os.Args[0]), "test_data", "system-tests")

	installerConfig, err := cli.NewInstallerConfig(fs, testDataPath)
	if err != nil {
		return nil, err
	}

	archiveConfig := cli.NewArchiveConfig(fs, testDataPath)
	deviceConfig := cli.NewDeviceConfig(fs, testDataPath)

	c := &config{
		archiveConfig:   archiveConfig,
		deviceConfig:    deviceConfig,
		installerConfig: installerConfig,
		buildConfig:     cli.NewBuildConfig(fs, archiveConfig, deviceConfig, os.Getenv("BUILDBUCKET_ID")),
	}

	fs.IntVar(&c.cycleCount, "cycle-count", 1, "How many cycles to run the test before completing (default is 1)")
	fs.DurationVar(&c.paveTimeout, "pave-timeout", 5*time.Minute, "Err if a pave takes longer than this time (default 5 minutes)")
	fs.DurationVar(&c.cycleTimeout, "cycle-timeout", 5*time.Minute, "Err if a test cycle takes longer than this time (default is 5 minutes)")
	fs.StringVar(&c.beforeInitScript, "before-init-script", "", "Run this script before initializing device for testing")
	fs.StringVar(&c.afterInitScript, "after-init-script", "", "Run this script after initializing device for testing")
	fs.StringVar(&c.afterTestScript, "after-test-script", "", "Run this script after a test step")
	fs.BoolVar(&c.useFlash, "use-flash", false, "Provision device using flashing instead of paving")

	return c, nil
}

func (c *config) validate() error {
	if err := c.buildConfig.Validate(); err != nil {
		return err
	}

	if err := c.installerConfig.Validate(); err != nil {
		return err
	}

	if err := c.deviceConfig.Validate(); err != nil {
		return err
	}

	if err := util.ValidatePath(c.afterInitScript); err != nil {
		return err
	}

	if err := util.ValidatePath(c.afterTestScript); err != nil {
		return err
	}

	return nil
}
