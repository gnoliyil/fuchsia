// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"go.fuchsia.dev/fuchsia/tools/botanist"
	"go.fuchsia.dev/fuchsia/tools/botanist/constants"
	"go.fuchsia.dev/fuchsia/tools/botanist/targets"
	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/lib/environment"
	"go.fuchsia.dev/fuchsia/tools/lib/ffxutil"
	"go.fuchsia.dev/fuchsia/tools/lib/flagmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/lib/osmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/serial"
	"go.fuchsia.dev/fuchsia/tools/lib/subprocess"
	"go.fuchsia.dev/fuchsia/tools/lib/syslog"
	"go.fuchsia.dev/fuchsia/tools/testing/testrunner"
	testrunnerconstants "go.fuchsia.dev/fuchsia/tools/testing/testrunner/constants"

	"github.com/google/subcommands"
	"golang.org/x/sync/errgroup"
)

// RunCommand is a Command implementation for booting a device and running a
// given command locally.
type RunCommand struct {
	// ConfigFile is the path to the target configurations.
	configFile string

	// DownloadManifest is the path we should write the package server's
	// download manifest to.
	downloadManifest string

	// ImageManifest is a path to an image manifest.
	imageManifest string

	// Netboot tells botanist to netboot (and not to pave).
	netboot bool

	// ZirconArgs are kernel command-line arguments to pass on boot.
	zirconArgs flagmisc.StringsValue

	// Timeout is the duration allowed for the command to finish execution.
	timeout time.Duration

	// syslogDir, if nonempty, is the directory in which system syslogs will be written.
	syslogDir string

	// SshKey is the path to a private SSH user key.
	sshKey string

	// serialLogDir, if nonempty, is the directory in which system serial logs will be written.
	serialLogDir string

	// RepoURL specifies the URL of a package repository.
	repoURL string

	// BlobURL optionally specifies the URL of where a package repository's blobs may be served from.
	// Defaults to $repoURL/blobs.
	blobURL string

	// localRepo specifies the path to a local package repository. If set,
	// botanist will spin up a package server to serve packages from this
	// repository.
	localRepo string

	// The path to the ffx tool.
	ffxPath string

	// The level of experimental ffx features to enable.
	//
	// The following levels enable the following ffx features:
	// 0 or greater: ffx emu
	// 1 or greater: ffx target flash, ffx bootloader boot
	// 2 or greater: ffx test, ffx target snapshot, keeps ffx output dir for debugging
	// 3: enables parallel test execution
	ffxExperimentLevel int

	// Any image overrides for boot.
	imageOverrides imageOverridesFlagValue

	// When true skips setting up the targets.
	skipSetup bool

	// Args passed to testrunner
	testrunnerOptions testrunner.Options
}

type imageOverridesFlagValue build.ImageOverrides

func (v *imageOverridesFlagValue) String() string {
	data, err := json.Marshal(v)
	if err != nil {
		return ""
	}
	return string(data)
}

func (v *imageOverridesFlagValue) Set(s string) error {
	return json.Unmarshal([]byte(s), &v)
}

// targetInfo is the schema for a JSON object used to communicate target
// information (device properties, serial paths, SSH properties, etc.) to
// subprocesses.

// LINT.IfChange
type targetInfo struct {
	// Nodename is the Fuchsia nodename of the target.
	Nodename string `json:"nodename"`

	// IPv4 is the IPv4 address of the target.
	IPv4 string `json:"ipv4"`

	// IPv6 is the IPv6 address of the target.
	IPv6 string `json:"ipv6"`

	// SerialSocket is the path to the serial socket, if one exists.
	SerialSocket string `json:"serial_socket"`

	// SSHKey is a path to a private key that can be used to access the target.
	SSHKey string `json:"ssh_key"`
}

// LINT.ThenChange(//src/testing/end_to_end/mobly_driver/api_mobly.py)

func (*RunCommand) Name() string {
	return "run"
}

func (*RunCommand) Usage() string {
	return `
botanist run [flags...] tests-file

flags:
`
}

func (*RunCommand) Synopsis() string {
	return fmt.Sprintf("boots a device and executes all tests found in the JSON [tests-file].")
}

func (r *RunCommand) SetFlags(f *flag.FlagSet) {
	f.StringVar(&r.configFile, "config", "", "path to file of device config")
	f.StringVar(&r.imageManifest, "images", "", "path to an image manifest")
	f.BoolVar(&r.netboot, "netboot", false, "if set, botanist will not pave; but will netboot instead")
	f.Var(&r.zirconArgs, "zircon-args", "kernel command-line arguments")
	f.DurationVar(&r.timeout, "timeout", 0, "duration allowed for the command to finish execution, a value of 0 (zero) will not impose a timeout.")
	f.StringVar(&r.syslogDir, "syslog-dir", "", "the directory to write all system logs to.")
	f.StringVar(&r.sshKey, "ssh", "", "file containing a private SSH user key; if not provided, a private key will be generated.")
	f.StringVar(&r.serialLogDir, "serial-log-dir", "", "the directory to write all serial logs to.")
	f.StringVar(&r.repoURL, "repo", "", "URL at which to configure a package repository; if the placeholder of \"localhost\" will be resolved and scoped as appropriate")
	f.StringVar(&r.blobURL, "blobs", "", "URL at which to serve a package repository's blobs; if the placeholder of \"localhost\" will be resolved and scoped as appropriate")
	f.StringVar(&r.localRepo, "local-repo", "", "path to a local package repository; the repo and blobs flags are ignored when this is set")
	f.StringVar(&r.ffxPath, "ffx", "", "Path to the ffx tool.")
	f.StringVar(&r.downloadManifest, "download-manifest", "", "Path to a manifest containing all package server downloads")
	f.IntVar(&r.ffxExperimentLevel, "ffx-experiment-level", 0, "The level of experimental features to enable. If -ffx is not set, this will have no effect.")
	f.BoolVar(&r.skipSetup, "skip-setup", false, "if set, botanist will not set up a target.")
	f.Var(&r.imageOverrides, "image-overrides", "A json struct following the ImageOverrides schema at //tools/build/tests.go with the names of the images to use from images.json.")

	// Parsing of testrunner options.
	f.StringVar(&r.testrunnerOptions.OutDir, "out-dir", "", "Optional path where a directory containing test results should be created.")
	f.StringVar(&r.testrunnerOptions.NsjailPath, "nsjail", "", "Optional path to an NsJail binary to use for linux host test sandboxing.")
	f.StringVar(&r.testrunnerOptions.NsjailRoot, "nsjail-root", "", "Path to the directory to use as the NsJail root directory")
	f.StringVar(&r.testrunnerOptions.LocalWD, "C", "", "Working directory of local testing subprocesses; if unset the current working directory will be used.")
	f.StringVar(&r.testrunnerOptions.SnapshotFile, "snapshot-output", "", "The output filename for the snapshot. This will be created in the output directory.")
	f.BoolVar(&r.testrunnerOptions.PrefetchPackages, "prefetch-packages", false, "Prefetch any test packages in the background.")
	f.BoolVar(&r.testrunnerOptions.UseSerial, "use-serial", false, "Use serial to run tests on the target.")
}

func (r *RunCommand) execute(ctx context.Context, args []string) error {
	ctx, cancel := context.WithCancel(ctx)
	if r.timeout != 0 {
		ctx, cancel = context.WithTimeout(ctx, r.timeout)
	}

	testsPath := args[0]

	go func() {
		<-ctx.Done()
		// Log the timeout for tefmocheck to detect it.
		if ctx.Err() == context.DeadlineExceeded {
			logger.Errorf(ctx, "%s (%s)", constants.CommandExceededTimeoutMsg, r.timeout)
		}
	}()
	defer cancel()

	if r.skipSetup {
		if err := testrunner.SetupAndExecute(ctx, r.testrunnerOptions, testsPath); err != nil {
			return fmt.Errorf("testrunner with flags: %v, with timeout: %s, failed: %w", r.testrunnerOptions, r.timeout, err)
		}
		return nil
	}

	// Parse targets out from the target configuration file.
	targetSlice, err := r.deriveTargetsFromFile(ctx)
	if err != nil {
		return err
	}
	// This is the primary target that a command will be run against and that
	// logs will be streamed from.
	t0 := targetSlice[0]
	ffxOutputsDir := filepath.Join(os.Getenv(testrunnerconstants.TestOutDirEnvKey), "ffx_outputs")
	removeFFXOutputsDir := false
	ffx, err := ffxutil.NewFFXInstance(ctx, r.ffxPath, "", []string{}, t0.Nodename(), t0.SSHKey(), ffxOutputsDir)
	if err != nil {
		return err
	}
	if ffx != nil {
		stdout, stderr, flush := botanist.NewStdioWriters(ctx)
		defer flush()
		ffx.SetStdoutStderr(stdout, stderr)
		cmd := ffx.Command("daemon", "start")
		daemonLog, err := osmisc.CreateFile(filepath.Join(ffxOutputsDir, "daemon.log"))
		if err != nil {
			return err
		}
		cmd.Stdout = daemonLog
		logger.Debugf(ctx, "%s", cmd.Args)
		if err := cmd.Start(); err != nil {
			return err
		}
		defer func() {
			// TODO(fxbug.dev/120758): Clean up daemon by sending a SIGTERM to the
			// process once that is supported.
			if err := ffx.Stop(); err != nil {
				logger.Errorf(ctx, "failed to stop ffx daemon: %s", err)
			}
			if err := cmd.Wait(); err != nil {
				logger.Errorf(ctx, "daemon process finished with err: %s", err)
			} else {
				logger.Debugf(ctx, "ffx daemon process finished")
			}
			if err := daemonLog.Close(); err != nil {
				logger.Errorf(ctx, "failed to close ffx daemon log: %s", err)
			}
			if removeFFXOutputsDir {
				os.RemoveAll(ffxOutputsDir)
			}
		}()
		if r.ffxExperimentLevel > 0 {
			if err := ffx.SetLogLevel(ctx, ffxutil.Trace); err != nil {
				return err
			}
		}
		if err := ffx.Run(ctx, "config", "env"); err != nil {
			return err
		}
		// ffxutil disables the mdns discovery if it can find a FUCHSIA_DEVICE_ADDR
		// to manually add to its targets. We should disable it from the start so
		// that it is off during target setup as well.
		if err := ffx.Run(ctx, "config", "set", "discovery.mdns.enabled", "false", "-l", "global"); err != nil {
			return err
		}
	}

	for _, t := range targetSlice {
		// Start serial servers for all targets. Will no-op for targets that
		// already have serial servers.
		if err := t.StartSerialServer(); err != nil {
			return err
		}
		// Attach an ffx instance for all targets. All ffx instances will use the same
		// config and daemon, but run commands against its own specified target.
		if ffx != nil {
			ffxForTarget, err := ffxutil.NewFFXInstance(ctx, r.ffxPath, "", []string{}, t.Nodename(), t.SSHKey(), ffxOutputsDir)
			if err != nil {
				return err
			}
			t.SetFFX(&targets.FFXInstance{ffxForTarget, r.ffxExperimentLevel}, ffx.Env())
		}
		t.SetImageOverrides(build.ImageOverrides(r.imageOverrides))
	}

	eg, ctx := errgroup.WithContext(ctx)
	if r.serialLogDir != "" {
		if err := os.Mkdir(r.serialLogDir, os.ModePerm); err != nil {
			return err
		}
		for _, t := range targetSlice {
			t := t
			eg.Go(func() error {
				logger.Debugf(ctx, "starting serial collection for target %s", t.Nodename())

				// Create a new file to capture the serial log for this nodename.
				serialLogName := fmt.Sprintf("%s_serial_log.txt", t.Nodename())
				// TODO(fxbug.dev/71529): Remove once there are no dependencies on this filename.
				if len(targetSlice) == 1 {
					serialLogName = "serial_log.txt"
				}
				serialLogPath := filepath.Join(r.serialLogDir, serialLogName)

				// Start capturing the serial log for this target.
				if err := t.CaptureSerialLog(serialLogPath); err != nil && ctx.Err() == nil {
					return err
				}
				return nil
			})
		}
	}

	// Run any preflights to prepare the testbed.
	if err := r.runPreflights(ctx); err != nil {
		return err
	}

	// Start up a local package server if one was requested.
	if r.localRepo != "" {
		var port int
		pkgSrvPort := os.Getenv(constants.PkgSrvPortKey)
		if pkgSrvPort == "" {
			logger.Warningf(ctx, "%s is empty, using default port %d", constants.PkgSrvPortKey, botanist.DefaultPkgSrvPort)
			port = botanist.DefaultPkgSrvPort
		} else {
			var err error
			port, err = strconv.Atoi(pkgSrvPort)
			if err != nil {
				return err
			}
		}
		pkgSrv, err := botanist.NewPackageServer(ctx, r.localRepo, r.repoURL, r.blobURL, r.downloadManifest, port)
		if err != nil {
			return err
		}
		defer pkgSrv.Close()
		// TODO(rudymathu): Once gcsproxy and remote package serving are deprecated, remove
		// the repoURL and blobURL from the command line flags.
		r.repoURL = pkgSrv.RepoURL
		r.blobURL = pkgSrv.BlobURL
	}
	// Disable usb mass storage to determine if it affects NUC stability.
	// TODO(rudymathu): Remove this once stability is achieved.
	r.zirconArgs = append(r.zirconArgs, "driver.usb_mass_storage.disable")

	// TODO(https://fxbug.dev/88370#c74): Remove this once CDC-ether flakiness
	// has been resolved.
	r.zirconArgs = append(r.zirconArgs, "driver.usb_cdc.log=debug")

	for _, t := range targetSlice {
		t := t
		eg.Go(func() error {
			if err := t.Wait(ctx); err != nil && err != targets.ErrUnimplemented && ctx.Err() == nil {
				return fmt.Errorf("target %s failed: %w", t.Nodename(), err)
			}
			return nil
		})
	}

	eg.Go(func() error {
		// Signal other goroutines to exit.
		defer cancel()

		startOpts := targets.StartOptions{
			Netboot:       r.netboot,
			ImageManifest: r.imageManifest,
			ZirconArgs:    r.zirconArgs,
		}

		if err := targets.StartTargets(ctx, startOpts, targetSlice); err != nil {
			return fmt.Errorf("%s: %w", constants.FailedToStartTargetMsg, err)
		}
		logger.Debugf(ctx, "successfully started all targets")

		defer func() {
			ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
			defer cancel()
			targets.StopTargets(ctx, targetSlice)
		}()

		// Create a testbed config file. We have to do this after starting the
		// targets so that we can get their IP addresses.
		testbedConfig, err := r.createTestbedConfig(targetSlice)
		if err != nil {
			return err
		}
		defer os.Remove(testbedConfig)

		if !r.netboot {
			for _, t := range targetSlice {
				t := t
				client, err := t.SSHClient()
				if err != nil {
					if err := r.dumpSyslogOverSerial(ctx, t.SerialSocketPath()); err != nil {
						logger.Errorf(ctx, err.Error())
					}
					return err
				}
				if r.repoURL != "" {
					if err := t.AddPackageRepository(client, r.repoURL, r.blobURL); err != nil {
						return err
					}
					logger.Debugf(ctx, "added package repo to target %s", t.Nodename())
				}
				if r.syslogDir != "" {
					if _, err := os.Stat(r.syslogDir); errors.Is(err, os.ErrNotExist) {
						if err := os.Mkdir(r.syslogDir, os.ModePerm); err != nil {
							return err
						}
					}
					go func() {
						syslogName := fmt.Sprintf("%s_syslog.txt", t.Nodename())
						// TODO(fxbug.dev/71529): Remove when there are no dependencies on this filename.
						if len(targetSlice) == 1 {
							syslogName = "syslog.txt"
						}
						syslogPath := filepath.Join(r.syslogDir, syslogName)
						if err := t.CaptureSyslog(client, syslogPath, r.repoURL, r.blobURL); err != nil && ctx.Err() == nil {
							logger.Errorf(ctx, "failed to capture syslog at %s: %s", syslogPath, err)
						}
					}()
				}
			}
		}
		err = r.runAgainstTarget(ctx, t0, testsPath, testbedConfig)
		// Cancel ctx to notify other goroutines that this routine has completed.
		// If another goroutine gets an error and the context is canceled, it
		// should return nil so that we always prioritize the result from this
		// goroutine.
		cancel()
		return err
	})

	err = eg.Wait()
	if err == nil && r.ffxExperimentLevel > 0 {
		// In the case of a successful run, remove the ffx_outputs dir which contains ffx logs.
		// These logs can be very large, so we should only upload them in the case of a failure.
		removeFFXOutputsDir = true
	}
	return err
}

// runPreflights runs opaque preflight commands passed to botanist from
// the calling infrastructure.
func (r *RunCommand) runPreflights(ctx context.Context) error {
	logger.Debugf(ctx, "checking for preflights")
	botfilePath := os.Getenv("SWARMING_BOT_FILE")
	if botfilePath == "" {
		return nil
	}
	data, err := os.ReadFile(botfilePath)
	if err != nil {
		return err
	}
	if len(data) == 0 {
		// There were no commands in the botfile, exit out.
		return nil
	}
	type preflightCommands struct {
		Commands [][]string `json:"commands"`
	}
	var cmds preflightCommands
	if err := json.Unmarshal(data, &cmds); err != nil {
		return err
	}
	runner := subprocess.Runner{
		Env: os.Environ(),
	}
	for _, c := range cmds.Commands {
		logger.Debugf(ctx, "running preflight %s", c)
		if err := runner.Run(ctx, c, subprocess.RunOptions{}); err != nil {
			return err
		}
	}
	// Some preflight commands can cause side effects that take up to 30s.
	time.Sleep(30 * time.Second)
	logger.Debugf(ctx, "done running preflights")
	return nil
}

// createTestbedConfig creates a configuration file that describes the targets
// attached and returns the path to the file.
func (r *RunCommand) createTestbedConfig(targetSlice []targets.Target) (string, error) {
	var testbedConfig []targetInfo
	for _, t := range targetSlice {
		cfg := targetInfo{
			Nodename:     t.Nodename(),
			SerialSocket: t.SerialSocketPath(),
		}
		if !r.netboot {
			cfg.SSHKey = t.SSHKey()
			if ipv4, err := t.IPv4(); err != nil {
				return "", err
			} else if ipv4 != nil {
				cfg.IPv4 = ipv4.String()
			}

			if ipv6, err := t.IPv6(); err != nil {
				return "", err
			} else if ipv6 != nil {
				cfg.IPv6 = ipv6.String()
			}
		}
		testbedConfig = append(testbedConfig, cfg)
	}

	data, err := json.Marshal(testbedConfig)
	if err != nil {
		return "", err
	}

	f, err := os.CreateTemp("", "testbed_config")
	if err != nil {
		return "", err
	}
	defer f.Close()
	if _, err := f.Write(data); err != nil {
		return "", err
	}
	return f.Name(), nil
}

// dumpSyslogOverSerial runs log_listener over serial to collect logs that may
// help with debugging. This is intended to be used when SSH connection fails to
// get some information about the failure mode prior to exiting.
func (r *RunCommand) dumpSyslogOverSerial(ctx context.Context, socketPath string) error {
	socket, err := serial.NewSocket(ctx, socketPath)
	if err != nil {
		return fmt.Errorf("newSerialSocket failed: %w", err)
	}
	defer socket.Close()
	if err := serial.RunDiagnostics(ctx, socket); err != nil {
		return fmt.Errorf("failed to run serial diagnostics: %w", err)
	}
	// Dump the existing syslog buffer. This may not work if pkg-resolver is not
	// up yet, in which case it will just print nothing.
	cmds := []serial.Command{
		{Cmd: []string{syslog.LogListener, "--dump_logs", "yes"}, SleepDuration: 5 * time.Second},
	}
	if err := serial.RunCommands(ctx, socket, cmds); err != nil {
		return fmt.Errorf("failed to dump syslog over serial: %w", err)
	}
	return nil
}

func (r *RunCommand) runAgainstTarget(ctx context.Context, t targets.Target, testsPath string, testbedConfig string) error {
	testrunnerEnv := map[string]string{
		constants.NodenameEnvKey:      t.Nodename(),
		constants.SerialSocketEnvKey:  t.SerialSocketPath(),
		constants.ECCableEnvKey:       os.Getenv(constants.ECCableEnvKey),
		constants.TestbedConfigEnvKey: testbedConfig,
	}

	// If |netboot| is true, then we assume that fuchsia is not provisioned
	// with a netstack; in this case, do not try to establish a connection.
	if !r.netboot {
		var addr net.IPAddr
		ipv6, err := t.IPv6()
		if err != nil {
			return err
		}
		if ipv6 != nil {
			addr = *ipv6
		}
		ipv4, err := t.IPv4()
		if err != nil {
			return err
		}
		if ipv4 != nil {
			addr.IP = ipv4
			addr.Zone = ""
		}

		testrunnerEnv[constants.DeviceAddrEnvKey] = addr.String()
		testrunnerEnv[constants.IPv4AddrEnvKey] = ipv4.String()
		testrunnerEnv[constants.IPv6AddrEnvKey] = ipv6.String()
		if t.UseFFX() {
			// Add the target address in order to skip MDNS discovery.
			if err := t.GetFFX().Run(ctx, "target", "add", addr.String(), "--nowait"); err != nil {
				return err
			}
		}
	}

	// One would assume this should only be provisioned when paving, but
	// there are some tests that attempt to SSH into a netbooted image that
	// has our SSH keys baked into it. Therefore, we add the SSH key to the
	// environment unconditionally. Additionally, some tools like FFX often
	// require the SSH key path to be absolute (https://fxbug.dev/101081).
	if t.SSHKey() != "" {
		absKeyPath, err := filepath.Abs(t.SSHKey())
		if err != nil {
			return err
		}
		testrunnerEnv[constants.SSHKeyEnvKey] = absKeyPath
	}

	// TODO(https://fxbug.dev/111922): testrunner does heavy use of env
	// variables. Setting these env variables is temporary until we refactor
	// testrunner to take these variables as arguments or flags.
	for k, v := range testrunnerEnv {
		err := os.Setenv(k, v)
		if err != nil {
			return fmt.Errorf("error setting env variable %s=%s. %w", k, v, err)
		}
	}
	if t.UseFFX() {
		setEnviron(t.FFXEnv())
		// TODO(fxbug.dev/113992): testrunner's use of ffx involves calls to a `ssh` host binary
		// which may not be available on the host. Put behind an experiment level until
		// the bug is fixed.
		if t.UseFFXExperimental(2) {
			r.testrunnerOptions.FFX = t.GetFFX().FFXInstance
			r.testrunnerOptions.FFXExperimentLevel = r.ffxExperimentLevel
		}
	}

	if err := testrunner.SetupAndExecute(ctx, r.testrunnerOptions, testsPath); err != nil {
		return fmt.Errorf("testrunner with flags: %v, with timeout: %s, failed: %w", r.testrunnerOptions, r.timeout, err)
	}
	return nil
}

// setEnviron sets |environ| into the os.Env.
// The string in the environ slice must be in the format "key=value".
func setEnviron(environ []string) {
	for _, env := range environ {
		keyval := strings.Split(env, "=")
		os.Setenv(keyval[0], keyval[1])
	}
}

func (r *RunCommand) Execute(ctx context.Context, f *flag.FlagSet, _ ...interface{}) subcommands.ExitStatus {
	args := f.Args()
	if len(args) == 0 {
		return subcommands.ExitUsageError
	}

	// If the TestOutDirEnvKey was set, that means botanist is being run in an infra
	// setting and thus needs an isolated environment.
	_, needsIsolatedEnv := os.LookupEnv(testrunnerconstants.TestOutDirEnvKey)
	cleanUp, err := environment.Ensure(needsIsolatedEnv)
	if err != nil {
		logger.Errorf(ctx, "failed to setup environment: %s", err)
		return subcommands.ExitFailure
	}
	defer cleanUp()

	r.blobURL = os.ExpandEnv(r.blobURL)
	r.repoURL = os.ExpandEnv(r.repoURL)
	if err := r.execute(ctx, args); err != nil {
		logger.Errorf(ctx, "%s", err)
		return subcommands.ExitFailure
	}
	return subcommands.ExitSuccess
}

func (r *RunCommand) deriveTargetsFromFile(ctx context.Context) ([]targets.Target, error) {
	opts := targets.Options{
		Netboot: r.netboot,
		SSHKey:  r.sshKey,
	}

	data, err := os.ReadFile(r.configFile)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", constants.ReadConfigFileErrorMsg, err)
	}
	var objs []json.RawMessage
	if err := json.Unmarshal(data, &objs); err != nil {
		return nil, fmt.Errorf("could not unmarshal config file as a JSON list: %w", err)
	}

	var targetSlice []targets.Target
	for _, obj := range objs {
		t, err := targets.DeriveTarget(ctx, obj, opts)
		if err != nil {
			return nil, err
		}
		targetSlice = append(targetSlice, t)
	}
	if len(targetSlice) == 0 {
		return nil, fmt.Errorf("no targets found")
	}
	return targetSlice, nil
}
