// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"bytes"
	"context"
	"debug/elf"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.fuchsia.dev/fuchsia/tools/debug/covargs"
	"go.fuchsia.dev/fuchsia/tools/debug/covargs/api/llvm"
	"go.fuchsia.dev/fuchsia/tools/debug/symbolize"
	"go.fuchsia.dev/fuchsia/tools/lib/cache"
	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/flagmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/lib/retry"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
	"golang.org/x/sync/errgroup"
)

const (
	shardSize              = 1000
	symbolCacheSize        = 100
	cloudFetchMaxAttempts  = 2
	cloudFetchRetryBackoff = 500 * time.Millisecond
	cloudFetchTimeout      = 60 * time.Second
	// TODO(fxbug.dev/121303): Remove this when debuginfod is supported in Rust toolchain.
	debuginfodSupportedVersion = "clang"
)

var (
	colors            color.EnableColor
	level             logger.LogLevel
	summaryFile       flagmisc.StringsValue
	buildIDDirPaths   flagmisc.StringsValue
	debuginfodServers flagmisc.StringsValue
	debuginfodCache   string
	symbolServers     flagmisc.StringsValue
	symbolCache       string
	coverageReport    bool
	dryRun            bool
	skipFunctions     bool
	outputDir         string
	llvmCov           string
	llvmProfdata      flagmisc.StringsValue
	outputFormat      string
	jsonOutput        string
	reportDir         string
	saveTemps         string
	basePath          string
	diffMappingFile   string
	compilationDir    string
	pathRemapping     flagmisc.StringsValue
	srcFiles          flagmisc.StringsValue
	numThreads        int
	jobs              int
)

func init() {
	colors = color.ColorAuto
	level = logger.InfoLevel

	flag.Var(&colors, "color", "can be never, auto, always")
	flag.Var(&level, "level", "can be fatal, error, warning, info, debug or trace")
	flag.Var(&summaryFile, "summary", "path to summary.json file. If given as `<path>=<version>`, the version should correspond "+
		"to the llvm-profdata required to run with the profiles from this summary.json")
	flag.Var(&buildIDDirPaths, "build-id-dir", "path to .build-id directory")
	flag.Var(&debuginfodServers, "debuginfod-server", "path to the debuginfod server")
	flag.StringVar(&debuginfodCache, "debuginfod-cache", "", "path to directory to store fetched debug binaries from debuginfodserver")
	flag.Var(&symbolServers, "symbol-server", "a GCS URL or bucket name that contains debug binaries indexed by build ID")
	flag.StringVar(&symbolCache, "symbol-cache", "", "path to directory to store cached debug binaries in")
	flag.BoolVar(&coverageReport, "coverage-report", true, "if set, generate a coverage report")
	flag.BoolVar(&dryRun, "dry-run", false, "if set the system prints out commands that would be run instead of running them")
	flag.BoolVar(&skipFunctions, "skip-functions", true, "if set, the coverage report enabled by the `report-dir` flag will not include function coverage")
	flag.StringVar(&outputDir, "output-dir", "", "the directory to output results to")
	flag.Var(&llvmProfdata, "llvm-profdata", "the location of llvm-profdata. If given as `<path>=<version>`, the version should correspond "+
		"to the summary containing profiles that this llvm-profdata tool should be used to run with")
	flag.StringVar(&llvmCov, "llvm-cov", "llvm-cov", "the location of llvm-cov")
	flag.StringVar(&outputFormat, "format", "html", "the output format used for llvm-cov")
	flag.StringVar(&jsonOutput, "json-output", "", "outputs profile information to the specified file")
	flag.StringVar(&saveTemps, "save-temps", "", "save temporary artifacts in a directory")
	flag.StringVar(&reportDir, "report-dir", "", "the directory to save the report to")
	flag.StringVar(&basePath, "base", "", "base path for source tree")
	flag.StringVar(&diffMappingFile, "diff-mapping", "", "path to diff mapping file")
	flag.StringVar(&compilationDir, "compilation-dir", "", "the directory used as a base for relative coverage mapping paths, passed through to llvm-cov")
	flag.Var(&pathRemapping, "path-equivalence", "<from>,<to> remapping of source file paths passed through to llvm-cov")
	flag.Var(&srcFiles, "src-file", "path to a source file to generate coverage for. If provided, only coverage for these files will be generated.\n"+
		"Multiple files can be specified with multiple instances of this flag.")
	flag.IntVar(&numThreads, "num-threads", 0, "number of processing threads")
	flag.IntVar(&jobs, "jobs", runtime.NumCPU(), "number of parallel jobs")
}

const llvmProfileSinkType = "llvm-profile"

func splitVersion(arg string) (version, value string) {
	version = ""
	s := strings.SplitN(arg, "=", 2)
	if len(s) > 1 {
		version = s[1]
	}
	return version, s[0]
}

// readSummary reads a summary file, and returns a mapping of unique profile to version.
func readSummary(summaryFiles []string) (map[string]string, error) {
	versionedSinks := make(map[string]runtests.DataSinkMap)

	for _, summaryFile := range summaryFiles {
		version, summaryFile := splitVersion(summaryFile)
		sinks, ok := versionedSinks[version]
		if !ok {
			sinks = make(runtests.DataSinkMap)
		}
		// TODO(phosek): process these in parallel using goroutines.
		file, err := os.Open(summaryFile)
		if err != nil {
			return nil, fmt.Errorf("cannot open %q: %w", summaryFile, err)
		}
		defer file.Close()

		var summary runtests.TestSummary
		if err := json.NewDecoder(file).Decode(&summary); err != nil {
			return nil, fmt.Errorf("cannot decode %q: %w", summaryFile, err)
		}

		dir := filepath.Dir(summaryFile)
		for _, detail := range summary.Tests {
			for name, data := range detail.DataSinks {
				for _, sink := range data {
					sinks[name] = append(sinks[name], runtests.DataSink{
						Name: sink.Name,
						File: filepath.Join(dir, sink.File),
					})
				}
			}
		}
		versionedSinks[version] = sinks
	}

	// Dedupe sinks.
	profiles := make(map[string]string)
	for version, summary := range versionedSinks {
		for _, sink := range summary[llvmProfileSinkType] {
			profiles[sink.File] = version
		}
	}

	return profiles, nil
}

type Action struct {
	Path string   `json:"cmd"`
	Args []string `json:"args"`
}

func (a Action) Run(ctx context.Context) ([]byte, error) {
	logger.Debugf(ctx, "%s\n", a.String())
	if !dryRun {
		return exec.Command(a.Path, a.Args...).CombinedOutput()
	}
	return nil, nil
}

func (a Action) String() string {
	var buf bytes.Buffer
	fmt.Fprint(&buf, a.Path)
	for _, arg := range a.Args {
		fmt.Fprintf(&buf, " %s", arg)
	}
	return buf.String()
}

// mergeProfiles merges raw profiles (.profraw) and returns a single indexed profile.
func mergeProfiles(ctx context.Context, tempDir string, profiles map[string]string, llvmProfDataVersions map[string]string) (string, error) {
	partitionedProfiles := make(map[string][]string)
	for profile, version := range profiles {
		partitionedProfiles[version] = append(partitionedProfiles[version], profile)
	}

	profdataFiles := []string{}
	for version, profileList := range partitionedProfiles {
		if len(profileList) == 0 {
			continue
		}

		// Make the llvm-profdata response file.
		profdataFile, err := os.Create(filepath.Join(tempDir, fmt.Sprintf("llvm-profdata%s.rsp", version)))
		if err != nil {
			return "", fmt.Errorf("creating llvm-profdata.rsp file: %w", err)
		}

		for _, profile := range profileList {
			fmt.Fprintf(profdataFile, "%s\n", profile)
		}
		profdataFile.Close()

		// Merge all raw profiles.
		mergedProfileFile := filepath.Join(tempDir, fmt.Sprintf("merged%s.profdata", version))
		args := []string{
			"merge",
			"--failure-mode=any",
			"--sparse",
			"--output", mergedProfileFile,
		}
		if numThreads != 0 {
			args = append(args, "--num-threads", strconv.Itoa(numThreads))
		}
		args = append(args, "@"+profdataFile.Name())

		// Find the associated llvm-profdata tool.
		llvmProfData, ok := llvmProfDataVersions[version]
		if !ok {
			return "", fmt.Errorf("no llvm-profdata has been specified for version %q", version)
		}
		mergeCmd := Action{Path: llvmProfData, Args: args}
		data, err := mergeCmd.Run(ctx)
		if err != nil {
			return "", fmt.Errorf("%s failed with %v:\n%s", mergeCmd.String(), err, string(data))
		}
		profdataFiles = append(profdataFiles, mergedProfileFile)
	}

	mergedProfileFile := filepath.Join(tempDir, "merged.profdata")
	args := []string{
		"merge",
		"--failure-mode=any",
		"--sparse",
		"--output", mergedProfileFile,
	}
	if numThreads != 0 {
		args = append(args, "--num-threads", strconv.Itoa(numThreads))
	}
	args = append(args, profdataFiles...)
	mergeCmd := Action{Path: llvmProfDataVersions[""], Args: args}
	data, err := mergeCmd.Run(ctx)
	if err != nil {
		return "", fmt.Errorf("%s failed with %v:\n%s", mergeCmd.String(), err, string(data))
	}

	return mergedProfileFile, nil
}

type profileReadingError struct {
	profile    string
	errMessage string
}

func (e *profileReadingError) Error() string {
	return fmt.Sprintf("cannot read profile %q: %q", e.profile, e.errMessage)
}

// readEmbeddedBuildId reads embedded build id from a profile by invoking llvm-profdata tool and returns it.
func readEmbeddedBuildId(ctx context.Context, tool string, profile string) (string, error) {
	args := []string{
		"show",
		"--binary-ids",
	}
	args = append(args, profile)
	readCmd := Action{Path: tool, Args: args}
	output, err := readCmd.Run(ctx)
	if err != nil {
		return "", &profileReadingError{profile, string(output)}
	}

	// Split the lines in llvm-profdata output, which should have the following lines for build ids.
	// Binary IDs:
	// 1696251c (This is an example for build id)
	splittedOutput := strings.Split((string(output)), "Binary IDs:")
	if len(splittedOutput) < 2 {
		return "", fmt.Errorf("invalid llvm-profdata output in profile %q: %w\n%s", profile, err, string(output))
	}

	embeddedBuildId := strings.TrimSpace(splittedOutput[len(splittedOutput)-1])
	if embeddedBuildId == "" {
		return "", fmt.Errorf("no build id found in profile %q:\n%s", profile, string(output))
	}
	// Check if embedded build id consists of hex characters.
	_, err = hex.DecodeString(embeddedBuildId)
	if err != nil {
		return "", fmt.Errorf("invalid build id in profile %q: %w", profile, err)
	}

	return embeddedBuildId, nil
}

type profileEntry struct {
	Profile string `json:"profile"`
	Module  string `json:"module"`
}

// createEntries creates a list of entries (profile and build id pairs) that is used to fetch binaries from symbol server.
func createEntries(ctx context.Context, profiles map[string]string, llvmProfDataVersions map[string]string) ([]profileEntry, error) {
	profileEntryChan := make(chan profileEntry, len(profiles))
	sems := make(chan struct{}, jobs)
	var eg errgroup.Group
	for profile, version := range profiles {
		profile := profile // capture range variable.
		version := version
		// Do not add the profiles that have debuginfod support because there is no need to fetch the associated binaries from symbol server for them.
		if len(debuginfodServers) > 0 && version == debuginfodSupportedVersion {
			continue
		}
		sems <- struct{}{}
		eg.Go(func() error {
			defer func() { <-sems }()

			// Find the associated llvm-profdata tool.
			llvmProfData, ok := llvmProfDataVersions[version]
			if !ok {
				return fmt.Errorf("no llvm-profdata has been specified for version %q", version)
			}

			// Read embedded build ids.
			embeddedBuildId, err := readEmbeddedBuildId(ctx, llvmProfData, profile)
			if err != nil {
				return err
			}
			profileEntryChan <- profileEntry{
				Profile: profile,
				Module:  embeddedBuildId,
			}
			return nil
		})
	}

	if err := eg.Wait(); err != nil {
		return nil, err
	}

	close(profileEntryChan)

	var entries []profileEntry
	for pe := range profileEntryChan {
		entries = append(entries, pe)
	}

	return entries, nil
}

func isInstrumented(filepath string) bool {
	file, err := os.Open(filepath)
	if err != nil {
		return false
	}
	defer file.Close()
	if elfFile, err := elf.NewFile(file); err == nil {
		return elfFile.Section("__llvm_prf_cnts") != nil
	}
	return false
}

// fetchFromSymbolServer returns all the binaries that are fetched from the symbol server when symbol server option is provided.
func fetchFromSymbolServer(ctx context.Context, mergedProfileFile string, tempDir string, entries *[]profileEntry) ([]symbolize.FileCloser, error) {
	var fileCache *cache.FileCache
	if len(symbolServers) > 0 {
		if symbolCache == "" {
			return nil, fmt.Errorf("-symbol-cache must be set if a symbol server is used")
		}
		var err error
		if fileCache, err = cache.GetFileCache(symbolCache, symbolCacheSize); err != nil {
			return nil, err
		}
	}

	var repo symbolize.CompositeRepo
	for _, dir := range buildIDDirPaths {
		repo.AddRepo(symbolize.NewBuildIDRepo(dir))
	}

	for _, symbolServer := range symbolServers {
		cloudRepo, err := symbolize.NewCloudRepo(ctx, symbolServer, fileCache)
		if err != nil {
			return nil, err
		}
		cloudRepo.SetTimeout(cloudFetchTimeout)
		repo.AddRepo(cloudRepo)
	}

	// Gather the set of modules and coverage files.
	modules := []symbolize.FileCloser{}
	files := make(chan symbolize.FileCloser)
	malformedModules := make(chan string)
	s := make(chan struct{}, jobs)
	var wg sync.WaitGroup
	for _, entry := range *entries {
		wg.Add(1)
		go func(module string) {
			defer wg.Done()
			s <- struct{}{}
			defer func() { <-s }()
			var file symbolize.FileCloser
			if err := retry.Retry(ctx, retry.WithMaxAttempts(retry.NewConstantBackoff(cloudFetchRetryBackoff), cloudFetchMaxAttempts), func() error {
				var err error
				file, err = repo.GetBuildObject(module)
				return err
			}, nil); err != nil {
				logger.Warningf(ctx, "module with build id %s not found: %v\n", module, err)
				return
			}
			if isInstrumented(file.String()) {
				// Run llvm-cov with the individual module to make sure it's valid.
				args := []string{
					"show",
					"-instr-profile", mergedProfileFile,
					"-summary-only",
				}
				for _, remapping := range pathRemapping {
					args = append(args, "-path-equivalence", remapping)
				}
				args = append(args, file.String())
				showCmd := Action{Path: llvmCov, Args: args}
				data, err := showCmd.Run(ctx)
				if err != nil {
					logger.Warningf(ctx, "module %s returned err %v:\n%s", module, err, string(data))
					file.Close()
					malformedModules <- module
				} else {
					files <- file
				}
			} else {
				file.Close()
			}
		}(entry.Module)
	}
	go func() {
		wg.Wait()
		close(malformedModules)
		close(files)
	}()
	var malformed []string
	go func() {
		for m := range malformedModules {
			malformed = append(malformed, m)
		}
	}()
	for f := range files {
		modules = append(modules, f)
	}

	// Write the malformed modules to a file in order to keep track of the tests affected by fxbug.dev/74189.
	if err := os.WriteFile(filepath.Join(tempDir, "malformed_binaries.txt"), []byte(strings.Join(malformed, "\n")), os.ModePerm); err != nil {
		return modules, fmt.Errorf("failed to write malformed binaries to a file: %w", err)
	}

	return modules, nil
}

// createLLVMCovResponseFile adds fetched binaries to the llvm-cov response file.
func createLLVMCovResponseFile(tempDir string, modules *[]symbolize.FileCloser) (string, error) {
	// Make the llvm-cov response file.
	covFile, err := os.Create(filepath.Join(tempDir, "llvm-cov.rsp"))
	if err != nil {
		return "", fmt.Errorf("creating llvm-cov.rsp file: %w", err)
	}

	for i, module := range *modules {
		// llvm-cov expects a positional arg representing the first
		// object file before it processes the rest of the positional
		// args as source files, so we don't use an -object flag with
		// the first file.
		if i == 0 {
			fmt.Fprintf(covFile, "%s\n", module.String())
		} else {
			fmt.Fprintf(covFile, "-object %s\n", module.String())
		}
	}

	if len(srcFiles) > 0 {
		fmt.Fprintf(covFile, "-sources\n")
		for _, srcFile := range srcFiles {
			fmt.Fprintf(covFile, "%s\n", srcFile)
		}
	}

	covFile.Close()
	return covFile.Name(), nil
}

// setDebugInfodEnvironmentVariables sets the necessary environment variables for debuginfod.
func setDebugInfodEnvironmentVariables(cmd *exec.Cmd) {
	// When debuginfod server is provided, set the DEBUGINFOD_URLS environment variable that is a string of a space-separated URLs.
	if len(debuginfodServers) > 0 {
		debuginfodUrls := strings.Join(debuginfodServers, " ")
		cmd.Env = os.Environ()
		cmd.Env = append(cmd.Env, "DEBUGINFOD_URLS="+debuginfodUrls)

		if debuginfodCache != "" {
			cmd.Env = append(cmd.Env, "DEBUGINFOD_CACHE_PATH="+debuginfodCache)
		}
	}
}

// showCoverageData shows coverage data by invoking llvm-cov show command.
func showCoverageData(ctx context.Context, mergedProfileFile string, covFile string) error {
	// Make the output directory.
	err := os.MkdirAll(outputDir, os.ModePerm)
	if err != nil {
		return fmt.Errorf("creating output dir %s: %w", outputDir, err)
	}

	// Produce HTML report.
	args := []string{
		"show",
		"-format", outputFormat,
		"-instr-profile", mergedProfileFile,
		"-output-dir", outputDir,
	}
	if compilationDir != "" {
		args = append(args, "-compilation-dir", compilationDir)
	}
	for _, remapping := range pathRemapping {
		args = append(args, "-path-equivalence", remapping)
	}

	args = append(args, "@"+covFile)
	showCmd := exec.Command(llvmCov, args...)
	logger.Debugf(ctx, "%s\n", showCmd)
	setDebugInfodEnvironmentVariables(showCmd)
	data, err := showCmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%v:\n%s", err, string(data))
	}
	logger.Debugf(ctx, "%s\n", string(data))

	return nil
}

// exportCoverageData exports coverage data by invoking llvm-cov export command.
func exportCoverageData(ctx context.Context, mergedProfileFile string, covFile string, tempDir string) error {
	// Make the export directory.
	err := os.MkdirAll(reportDir, os.ModePerm)
	if err != nil {
		return fmt.Errorf("creating export dir %s: %w", reportDir, err)
	}

	stderrFilename := filepath.Join(tempDir, "llvm-cov.stderr.log")
	stderrFile, err := os.Create(stderrFilename)
	if err != nil {
		return fmt.Errorf("creating export %q: %w", stderrFilename, err)
	}
	defer stderrFile.Close()

	// Export data in machine readable format.
	var b bytes.Buffer
	args := []string{
		"export",
		"-instr-profile", mergedProfileFile,
		"-skip-expansions",
	}
	if skipFunctions {
		args = append(args, "-skip-functions")
	}
	for _, remapping := range pathRemapping {
		args = append(args, "-path-equivalence", remapping)
	}

	args = append(args, "@"+covFile)
	exportCmd := exec.Command(llvmCov, args...)
	exportCmd.Stdout = &b
	exportCmd.Stderr = stderrFile
	setDebugInfodEnvironmentVariables(exportCmd)

	if err := exportCmd.Run(); err != nil {
		return fmt.Errorf("failed to export: %w", err)
	}

	coverageFilename := filepath.Join(tempDir, "coverage.json")
	if err := os.WriteFile(coverageFilename, b.Bytes(), 0644); err != nil {
		return fmt.Errorf("writing coverage %q: %w", coverageFilename, err)
	}

	if coverageReport {
		var export llvm.Export
		if err := json.NewDecoder(&b).Decode(&export); err != nil {
			return fmt.Errorf("failed to load the exported file: %w", err)
		}

		var mapping *covargs.DiffMapping
		if diffMappingFile != "" {
			file, err := os.Open(diffMappingFile)
			if err != nil {
				return fmt.Errorf("cannot open %q: %w", diffMappingFile, err)
			}
			defer file.Close()

			if err := json.NewDecoder(file).Decode(mapping); err != nil {
				return fmt.Errorf("failed to load the diff mapping file: %w", err)
			}
		}

		files, err := covargs.ConvertFiles(&export, basePath, mapping)
		if err != nil {
			return fmt.Errorf("failed to convert files: %w", err)
		}

		if _, err := covargs.SaveReport(files, shardSize, reportDir); err != nil {
			return fmt.Errorf("failed to save report: %w", err)
		}
	}

	return nil
}

func process(ctx context.Context) error {
	llvmProfDataVersions := make(map[string]string)
	for _, profdata := range llvmProfdata {
		version, tool := splitVersion(profdata)
		llvmProfDataVersions[version] = tool
	}
	if _, ok := llvmProfDataVersions[""]; !ok {
		return fmt.Errorf("missing default llvm-profdata tool path")
	}

	versionedProfiles, err := readSummary(summaryFile)
	if err != nil {
		return fmt.Errorf("failed to read summary file: %w", err)
	}

	tempDir := saveTemps
	if saveTemps == "" {
		tempDir, err = os.MkdirTemp(saveTemps, "covargs")
		if err != nil {
			return fmt.Errorf("cannot create temporary dir: %w", err)
		}
		defer os.RemoveAll(tempDir)
	}

	mergedProfileFile, err := mergeProfiles(ctx, tempDir, versionedProfiles, llvmProfDataVersions)
	if err != nil {
		return fmt.Errorf("failed to merge profiles: %w", err)
	}

	entries, err := createEntries(ctx, versionedProfiles, llvmProfDataVersions)
	if jsonOutput != "" {
		file, err := os.Create(jsonOutput)
		if err != nil {
			return fmt.Errorf("creating profile output file: %w", err)
		}
		defer file.Close()
		if err := json.NewEncoder(file).Encode(entries); err != nil {
			return fmt.Errorf("writing profile information: %w", err)
		}
	}

	modules, err := fetchFromSymbolServer(ctx, mergedProfileFile, tempDir, &entries)
	// Delete all the binary files that are fetched from symbol server.
	// Symbolize.FileCloser holds a reference to a file and deletes it after Close() is called.
	for _, f := range modules {
		defer f.Close()
	}
	if err != nil {
		return fmt.Errorf("failed to fetch from symbol server: %w", err)
	}

	covFile, err := createLLVMCovResponseFile(tempDir, &modules)
	if err != nil {
		return fmt.Errorf("failed to create llvm-cov response file: %w", err)
	}

	if outputDir != "" {
		if err = showCoverageData(ctx, mergedProfileFile, covFile); err != nil {
			return fmt.Errorf("failed to show coverage data: %w", err)
		}
	}

	if reportDir != "" {
		if err = exportCoverageData(ctx, mergedProfileFile, covFile, tempDir); err != nil {
			return fmt.Errorf("failed to export coverage data: %w", err)
		}
	}

	return nil
}

func main() {
	flag.Parse()
	log := logger.NewLogger(level, color.NewColor(colors), os.Stdout, os.Stderr, "")
	ctx := logger.WithLogger(context.Background(), log)

	if err := process(ctx); err != nil {
		log.Errorf("%v\n", err)
		os.Exit(1)
	}
}
