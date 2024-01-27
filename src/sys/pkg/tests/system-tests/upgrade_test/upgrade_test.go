// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package upgrade

import (
	"bytes"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"testing"
	"time"

	"go.fuchsia.dev/fuchsia/src/sys/pkg/tests/system-tests/check"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/tests/system-tests/flash"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/tests/system-tests/pave"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/tests/system-tests/script"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/artifacts"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/device"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/errutil"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/packages"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/sl4f"
	"go.fuchsia.dev/fuchsia/src/testing/host-target-testing/util"
	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

var c *config

func TestMain(m *testing.M) {
	log.SetPrefix("upgrade-test: ")
	log.SetFlags(log.Ldate | log.Ltime | log.LUTC | log.Lshortfile)

	var err error
	c, err = newConfig(flag.CommandLine)
	if err != nil {
		log.Fatalf("failed to create config: %s", err)
	}

	flag.Parse()

	if err = c.validate(); err != nil {
		log.Fatalf("config is invalid: %s", err)
	}

	os.Exit(m.Run())
}

func TestOTA(t *testing.T) {
	ctx := context.Background()
	l := logger.NewLogger(
		logger.TraceLevel,
		color.NewColor(color.ColorAuto),
		os.Stdout,
		os.Stderr,
		"upgrade-test: ")
	l.SetFlags(logger.Ldate | logger.Ltime | logger.LUTC | logger.Lshortfile)
	ctx = logger.WithLogger(ctx, l)

	defer c.installerConfig.Shutdown(ctx)

	deviceClient, err := c.deviceConfig.NewDeviceClient(ctx)
	if err != nil {
		t.Fatalf("failed to create ota test client: %v", err)
	}
	defer deviceClient.Close()

	if err := doTest(ctx, deviceClient); err != nil {
		logger.Errorf(ctx, "test failed: %v", err)
		errutil.HandleError(ctx, c.deviceConfig.SerialSocketPath, err)
		t.Fatal(err)
	}
}

func doTest(ctx context.Context, deviceClient *device.Client) error {
	outputDir, cleanup, err := c.archiveConfig.OutputDir()
	if err != nil {
		return fmt.Errorf("failed to get output directory: %w", err)
	}
	defer cleanup()

	l := logger.NewLogger(
		logger.TraceLevel,
		color.NewColor(color.ColorAuto),
		os.Stdout,
		os.Stderr,
		device.NewEstimatedMonotonicTime(deviceClient, "upgrade-test: "),
	)
	l.SetFlags(logger.Ldate | logger.Ltime | logger.LUTC | logger.Lshortfile)
	ctx = logger.WithLogger(ctx, l)

	downgradeBuild, err := c.downgradeBuildConfig.GetBuild(ctx, deviceClient, outputDir)
	if err != nil {
		return fmt.Errorf("failed to get downgrade build: %w", err)
	}

	upgradeBuild, err := c.upgradeBuildConfig.GetBuild(ctx, deviceClient, outputDir)
	if err != nil {
		return fmt.Errorf("failed to get upgrade build: %w", err)
	}

	// Adapt the builds for the device.
	if downgradeBuild != nil {
		downgradeBuild, err = c.installerConfig.ConfigureBuild(ctx, deviceClient, downgradeBuild)
		if err != nil {
			return fmt.Errorf("failed to configure downgrade build for device: %w", err)
		}

		if downgradeBuild == nil {
			return fmt.Errorf("installer did not configure a downgrade build")
		}
	}

	if upgradeBuild != nil {
		upgradeBuild, err = c.installerConfig.ConfigureBuild(ctx, deviceClient, upgradeBuild)
		if err != nil {
			return fmt.Errorf("failed to configure upgrade build for device: %w", err)
		}

		if upgradeBuild == nil {
			return fmt.Errorf("installer did not configure an upgrade build")
		}
	}

	chainedBuilds, err := c.chainedBuildConfig.GetBuilds(ctx, deviceClient)
	if err != nil {
		return fmt.Errorf("failed to get chained builds: %w", err)
	}
	logger.Infof(ctx, "Chained build %s", fmt.Sprint(chainedBuilds))

	for i, build := range chainedBuilds {
		build, err = c.installerConfig.ConfigureBuild(ctx, deviceClient, build)
		if err != nil {
			return fmt.Errorf("failed to configure build for device: %w", err)
		}

		if build == nil {
			return fmt.Errorf("installer did not configure a build")
		}
		chainedBuilds[i] = build
	}

	if downgradeBuild == nil && len(chainedBuilds) > 0 {
		downgradeBuild = chainedBuilds[0]
	}

	ch := make(chan *sl4f.Client, 1)
	if err := util.RunWithTimeout(ctx, c.paveTimeout, func() error {
		rpcClient, err := initializeDevice(ctx, deviceClient, downgradeBuild)
		ch <- rpcClient
		return err
	}); err != nil {
		err = fmt.Errorf("device failed to initialize: %w", err)
		errutil.HandleError(ctx, c.deviceConfig.SerialSocketPath, err)
		return err
	}

	rpcClient := <-ch
	defer func() {
		if rpcClient != nil {
			rpcClient.Close()
		}
	}()

	if upgradeBuild != nil {
		return testOTAs(ctx, deviceClient, []artifacts.Build{upgradeBuild}, &rpcClient)
	}

	return testOTAs(ctx, deviceClient, chainedBuilds[1:], &rpcClient)
}

func testOTAs(
	ctx context.Context,
	device *device.Client,
	builds []artifacts.Build,
	rpcClient **sl4f.Client,
) error {
	for i := uint(1); i <= c.cycleCount; i++ {
		logger.Infof(ctx, "OTA Attempt %d", i)

		for index, build := range builds {
			checkPrime := false
			if index == len(builds)-1 {
				checkPrime = true
			}

			if err := util.RunWithTimeout(ctx, c.cycleTimeout, func() error {
				return doTestOTAs(ctx, device, build, rpcClient, checkPrime)
			}); err != nil {
				return fmt.Errorf("OTA Attempt %d failed: %w", i, err)
			}
		}
	}

	return nil
}

func doTestOTAs(
	ctx context.Context,
	device *device.Client,
	build artifacts.Build,
	rpcClient **sl4f.Client,
	checkPrime bool,
) error {
	logger.Infof(ctx, "Starting OTA test cycle. Time out in %s", c.cycleTimeout)

	startTime := time.Now()

	ffx, err := c.deviceConfig.FFXTool()
	if err != nil {
		return fmt.Errorf("error getting FFXTool: %w", err)
	}
	repo, err := build.GetPackageRepository(ctx, artifacts.PrefetchBlobs, ffx)
	if err != nil {
		return fmt.Errorf("error getting repository: %w", err)
	}

	// Install version N on the device if it is not already on that version.
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle(ctx)
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %w", err)
	}

	// Attempt to check if the device is up-to-date, up to downgradeOTAAttempts times.
	// We retry this since some downgrade builds contain bugs which make them spuriously reboot
	// See fxbug.dev/109811 for more details
	var upToDate bool
	var lastError error
	for attempt := uint(1); attempt <= c.downgradeOTAAttempts; attempt++ {
		logger.Infof(ctx, "checking device version (attempt %d of %d)", attempt, c.downgradeOTAAttempts)
		upToDate, lastError = check.IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
		if lastError == nil {
			logger.Infof(ctx, "Got device version, upToDate: %t", upToDate)
			break
		}

		if attempt == c.downgradeOTAAttempts {
			return fmt.Errorf(
				"OTA from N-1 -> N failed to check if device is up to date after %d attempts: Last error: %w",
				c.downgradeOTAAttempts,
				lastError,
			)
		} else {
			logger.Warningf(
				ctx,
				"failed to check if device up to date, trying again %d times:: %v",
				c.downgradeOTAAttempts-attempt,
				lastError,
			)
		}

		// Reset our client state since the device has _potentially_ rebooted
		device.Close()
		newClient, err := c.deviceConfig.NewDeviceClient(ctx)
		if err != nil {
			return fmt.Errorf("failed to create ota test client: %w", err)
		}
		*device = *newClient
	}

	if !upToDate {
		// Attempt an N-1 -> N OTA, up to downgradeOTAAttempts times.
		// We optionally retry this OTA because some downgrade builds contain bugs which make them
		// spuriously reboot. Those builds are already cut, but we still need to test them.
		// See fxbug.dev/109811 for more details.
		for attempt := uint(1); attempt <= c.downgradeOTAAttempts; attempt++ {
			logger.Infof(ctx, "starting OTA from N-1 -> N test, attempt %d of %d", attempt, c.downgradeOTAAttempts)
			otaTime := time.Now()
			if lastError = systemOTA(ctx, device, rpcClient, repo, true); lastError == nil {
				logger.Infof(ctx, "OTA from N-1 -> N successful in %s", time.Now().Sub(otaTime))
				break
			}

			if attempt == c.downgradeOTAAttempts {
				return fmt.Errorf(
					"OTA from N-1 -> N failed after %d attempts: Last error: %w",
					c.downgradeOTAAttempts,
					lastError,
				)
			} else {
				logger.Warningf(
					ctx,
					"OTA from N-1 -> N failed, trying again %d times: %v",
					c.downgradeOTAAttempts-attempt,
					lastError)
			}

			// Reset our client state since the device has _potentially_ rebooted
			device.Close()
			newClient, err := c.deviceConfig.NewDeviceClient(ctx)
			if err != nil {
				return fmt.Errorf("failed to create ota test client: %w", err)
			}
			*device = *newClient

			if rpcClient != nil && *rpcClient != nil {
				(*rpcClient).Close()
				*rpcClient = nil
			}

			*rpcClient, err = device.StartRpcSession(ctx, repo)
			if err != nil {
				return fmt.Errorf("unable to connect to sl4f while retrying OTA: %w", err)
			}
		}
	}

	if !checkPrime {
		return nil
	}

	expectedSystemImageMerkle, err = repo.LookupUpdatePrimeSystemImageMerkle(ctx)
	if err != nil {
		logger.Infof(ctx, "No prime package info")
		return nil
	}

	logger.Infof(ctx, "starting OTA N -> N' test")
	otaTime := time.Now()
	if err := systemPrimeOTA(ctx, device, rpcClient, repo, false); err != nil {
		return fmt.Errorf("OTA from N -> N' failed: %w", err)
	}
	logger.Infof(ctx, "OTA from N -> N' successful in %s", time.Now().Sub(otaTime))

	logger.Infof(ctx, "starting OTA N' -> N test")
	otaTime = time.Now()
	if err := systemOTA(ctx, device, rpcClient, repo, false); err != nil {
		return fmt.Errorf("OTA from N' -> N failed: %w", err)
	}
	logger.Infof(ctx, "OTA from N' -> N successful in %s", time.Now().Sub(otaTime))
	logger.Infof(ctx, "OTA cycle sucessful in %s", time.Now().Sub(startTime))

	return nil
}

func initializeDevice(
	ctx context.Context,
	device *device.Client,
	build artifacts.Build,
) (*sl4f.Client, error) {
	logger.Infof(ctx, "Initializing device")

	startTime := time.Now()

	var repo *packages.Repository
	var expectedSystemImageMerkle string
	var err error

	if build != nil {
		ffx, err := c.deviceConfig.FFXTool()
		if err != nil {
			return nil, fmt.Errorf("error getting FFXTool: %w", err)
		}
		// We don't need to prefetch all the blobs, since we only use a subset of
		// packages from the repository, like run, sl4f.
		repo, err = build.GetPackageRepository(ctx, artifacts.LazilyFetchBlobs, ffx)
		if err != nil {
			return nil, fmt.Errorf("error getting downgrade repository: %w", err)
		}

		expectedSystemImageMerkle, err = repo.LookupUpdateSystemImageMerkle(ctx)
		if err != nil {
			return nil, fmt.Errorf("error extracting expected system image merkle: %w", err)
		}
	}

	if err := script.RunScript(ctx, device, repo, nil, c.beforeInitScript); err != nil {
		return nil, fmt.Errorf("failed to run before-init-script: %w", err)
	}

	if build != nil {
		// Only pave if the device is not running the expected version.
		upToDate, err := check.IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
		if err != nil {
			return nil, fmt.Errorf("failed to check if up to date during initialization: %w", err)
		}
		if upToDate {
			logger.Infof(ctx, "device already up to date")
		} else {
			if c.useFlash {
				if err := flash.FlashDevice(ctx, device, build); err != nil {
					return nil, fmt.Errorf("failed to flash device during initialization: %w", err)
				}
			} else {
				if err := pave.PaveDevice(ctx, device, build); err != nil {
					return nil, fmt.Errorf("failed to pave device during initialization: %w", err)
				}
			}
		}
	}

	// Creating a sl4f.Client requires knowing the build currently running
	// on the device, which not all test cases know during start. Store the
	// one true client here, and pass around pointers to the various
	// functions that may use it or device.Client to interact with the
	// target. All OTA attempts must first Close and nil out an existing
	// rpcClient and replace it with a new one after reboot. The final
	// rpcClient, if present, will be closed by the defer here.
	var rpcClient *sl4f.Client
	var expectedConfig *sl4f.Configuration
	if build != nil {
		rpcClient, err = device.StartRpcSession(ctx, repo)
		if err != nil {
			return nil, fmt.Errorf("unable to connect to sl4f after pave: %w", err)
		}

		// We always boot into the A partition after a pave.
		config := sl4f.ConfigurationA
		expectedConfig = &config
	}

	if err := check.ValidateDevice(
		ctx,
		device,
		rpcClient,
		expectedSystemImageMerkle,
		expectedConfig,
		true,
	); err != nil {
		if rpcClient != nil {
			rpcClient.Close()
		}
		return nil, fmt.Errorf("failed to validate during initialization: %w", err)
	}

	if err := script.RunScript(ctx, device, repo, &rpcClient, c.afterInitScript); err != nil {
		return nil, fmt.Errorf("failed to run after-init-script: %w", err)
	}

	logger.Infof(ctx, "initialization successful in %s", time.Now().Sub(startTime))

	return rpcClient, nil
}

func systemOTA(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	checkABR bool,
) error {
	expectedSystemImageMerkle, err := repo.LookupUpdateSystemImageMerkle(ctx)
	if err != nil {
		return fmt.Errorf("error extracting expected system image merkle: %w", err)
	}

	return otaToPackage(
		ctx,
		device,
		rpcClient,
		repo,
		expectedSystemImageMerkle,
		"fuchsia-pkg://fuchsia.com/update/0",
		checkABR,
	)
}

func systemPrimeOTA(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	checkABR bool,
) error {
	return buildAndOtaToPackage(
		ctx,
		device,
		rpcClient,
		repo,
		checkABR,
	)
}

func buildAndOtaToPackage(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	checkABR bool,
) error {
	avbTool, err := c.installerConfig.AVBTool()
	if err != nil {
		return fmt.Errorf("failed to intialize AVBTool: %q", err)
	}

	zbiTool, err := c.installerConfig.ZBITool()
	if err != nil {
		return fmt.Errorf("failed to intialize ZBITool: %q", err)
	}

	systemImagePrime2 := "system_image_prime2/0"
	systemImagePrime2Pkg, err := repo.EditPackage(ctx, "system_image/0", systemImagePrime2, func(tempDir string) error {
		newResource := "Hello World!"
		contents := bytes.NewReader([]byte(newResource))
		data, err := io.ReadAll((contents))
		if err != nil {
			return fmt.Errorf("failed to read new content %q %w", systemImagePrime2, err)
		}

		tempPath := filepath.Join(tempDir, "dummy2.txt")
		if err := os.MkdirAll(filepath.Dir(tempPath), os.ModePerm); err != nil {
			return fmt.Errorf("failed to create parent directories for %q: %w", tempPath, err)
		}

		if err := os.WriteFile(tempPath, data, 0644); err != nil {
			return fmt.Errorf("failed to write new data for %q to %q: %w", systemImagePrime2, tempPath, err)
		}

		return nil
	})

	if err != nil {
		return fmt.Errorf("failed to create the %q pacakge: %w", systemImagePrime2, err)
	}

	systemImagePrimeMerkle := systemImagePrime2Pkg.Merkle()

	_, err = repo.EditUpdatePackageWithNewSystemImageMerkle(
		ctx,
		avbTool,
		zbiTool,
		systemImagePrimeMerkle,
		"update/0",
		"update_prime2/0",
		c.bootfsCompression,
		func(path string) error {
			return nil
		},
	)

	return otaToPackage(
		ctx,
		device,
		rpcClient,
		repo,
		systemImagePrimeMerkle,
		"fuchsia-pkg://fuchsia.com/update_prime2/0",
		checkABR,
	)
}

func otaToPackage(
	ctx context.Context,
	device *device.Client,
	rpcClient **sl4f.Client,
	repo *packages.Repository,
	expectedSystemImageMerkle string,
	updatePackageURL string,
	checkABR bool,
) error {
	expectedConfig, err := check.DetermineTargetABRConfig(ctx, *rpcClient)
	if err != nil {
		return fmt.Errorf("error determining target config: %w", err)
	}

	upToDate, err := check.IsDeviceUpToDate(ctx, device, expectedSystemImageMerkle)
	if err != nil {
		return fmt.Errorf("failed to check if device is up to date: %w", err)
	}
	if upToDate {
		return fmt.Errorf("device already updated to the expected version %q", expectedSystemImageMerkle)
	}

	u, err := c.installerConfig.Updater(repo, updatePackageURL)
	if err != nil {
		return fmt.Errorf("failed to create updater: %w", err)
	}

	if err := u.Update(ctx, device); err != nil {
		return fmt.Errorf("failed to download OTA: %w", err)
	}

	logger.Infof(ctx, "Validating device")

	// Disconnect from sl4f since we rebooted the device.
	//
	// FIXME(47145) To avoid fxbug.dev/47145, we need to delay
	// disconnecting from sl4f until after we reboot the device. Otherwise
	// we risk leaving the ssh session in a bad state.
	if *rpcClient != nil {
		(*rpcClient).Close()
		*rpcClient = nil
	}

	*rpcClient, err = device.StartRpcSession(ctx, repo)
	if err != nil {
		return fmt.Errorf("unable to connect to sl4f after OTA: %w", err)
	}
	if err := check.ValidateDevice(
		ctx,
		device,
		*rpcClient,
		expectedSystemImageMerkle,
		expectedConfig,
		checkABR,
	); err != nil {
		return fmt.Errorf("failed to validate after OTA: %w", err)
	}

	if err := script.RunScript(ctx, device, repo, rpcClient, c.afterTestScript); err != nil {
		return fmt.Errorf("failed to run test script after OTA: %w", err)
	}

	return nil
}
