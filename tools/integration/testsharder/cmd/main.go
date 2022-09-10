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
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/integration/testsharder"
	"go.fuchsia.dev/fuchsia/tools/lib/color"
	"go.fuchsia.dev/fuchsia/tools/lib/flagmisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
)

func usage() {
	fmt.Printf(`testsharder [flags]

Shards tests produced by a build.
`)
}

type testsharderFlags struct {
	buildDir                       string
	outputFile                     string
	tags                           flagmisc.StringsValue
	modifiersPath                  string
	targetTestCount                int
	targetDurationSecs             int
	perTestTimeoutSecs             int
	maxShardsPerEnvironment        int
	affectedTestsPath              string
	affectedTestsMaxAttempts       int
	affectedTestsMultiplyThreshold int
	affectedOnly                   bool
	realmLabel                     string
	hermeticDeps                   bool
	imageDeps                      bool
	pave                           bool
	skipUnaffected                 bool
	perShardPackageRepos           bool
	cacheTestPackages              bool
}

func parseFlags() testsharderFlags {
	var flags testsharderFlags
	flag.StringVar(&flags.buildDir, "build-dir", "", "path to the fuchsia build directory root (required)")
	flag.StringVar(&flags.outputFile, "output-file", "", "path to a file which will contain the shards as JSON, default is stdout")
	flag.Var(&flags.tags, "tag", "environment tags on which to filter; only the tests that match all tags will be sharded")
	flag.StringVar(&flags.modifiersPath, "modifiers", "", "path to the json manifest containing tests to modify")
	flag.IntVar(&flags.targetDurationSecs, "target-duration-secs", 0, "approximate duration that each shard should run in")
	flag.IntVar(&flags.maxShardsPerEnvironment, "max-shards-per-env", 8, "maximum shards allowed per environment. If <= 0, no max will be set")
	// TODO(fxbug.dev/10456): Support different timeouts for different tests.
	flag.IntVar(&flags.perTestTimeoutSecs, "per-test-timeout-secs", 0, "per-test timeout, applied to all tests. If <= 0, no timeout will be set")
	// Despite being a misnomer, this argument is still called -max-shard-size
	// for legacy reasons. If it becomes confusing, we can create a new
	// target_test_count fuchsia.proto field and do a soft transition with the
	// recipes to start setting the renamed argument instead.
	flag.IntVar(&flags.targetTestCount, "max-shard-size", 0, "target number of tests per shard. If <= 0, will be ignored. Otherwise, tests will be placed into more, smaller shards")
	flag.StringVar(&flags.affectedTestsPath, "affected-tests", "", "path to a file containing names of tests affected by the change being tested. One test name per line.")
	flag.IntVar(&flags.affectedTestsMaxAttempts, "affected-tests-max-attempts", 2, "maximum attempts for each affected test. Only applied to tests that are not multiplied")
	flag.IntVar(&flags.affectedTestsMultiplyThreshold, "affected-tests-multiply-threshold", 0, "if there are <= this many tests in -affected-tests, they may be multplied "+
		"(modified to run many times in a separate shard), but only be multiplied if allowed by certain constraints designed to minimize false rejections and bot demand.")
	flag.BoolVar(&flags.affectedOnly, "affected-only", false, "whether to create test shards for only the affected tests found in either the modifiers file or the affected-tests file.")
	flag.StringVar(&flags.realmLabel, "realm-label", "", "applies this realm label to the output sharded json file generated by testsharder. If empty, no realm label is applied.")
	flag.BoolVar(&flags.hermeticDeps, "hermetic-deps", false, "whether to add all the images and blobs used by the shard as dependencies")
	flag.BoolVar(&flags.imageDeps, "image-deps", false, "whether to add all the images used by the shard as dependencies")
	flag.BoolVar(&flags.pave, "pave", false, "whether the shards generated should pave or netboot fuchsia")
	flag.BoolVar(&flags.skipUnaffected, "skip-unaffected", false, "whether the shards should ignore hermetic, unaffected tests")
	flag.BoolVar(&flags.perShardPackageRepos, "per-shard-package-repos", false, "whether to construct a local package repo for each shard")
	flag.BoolVar(&flags.cacheTestPackages, "cache-test-packages", false, "whether the test packages should be cached on disk in the local package repo")
	flag.Usage = usage

	flag.Parse()

	return flags
}

func main() {
	l := logger.NewLogger(logger.ErrorLevel, color.NewColor(color.ColorAuto), os.Stdout, os.Stderr, "")
	// testsharder is expected to complete quite quickly, so it's generally not
	// useful to include timestamps in logs. File names can be helpful though.
	l.SetFlags(logger.Lshortfile)
	ctx := logger.WithLogger(context.Background(), l)
	ctx, cancel := signal.NotifyContext(ctx, syscall.SIGTERM, syscall.SIGINT)
	defer cancel()

	if err := mainImpl(ctx); err != nil {
		logger.Fatalf(ctx, err.Error())
	}
}

func mainImpl(ctx context.Context) error {
	flags := parseFlags()

	if flags.buildDir == "" {
		return fmt.Errorf("must specify a Fuchsia build output directory")
	}

	m, err := build.NewModules(flags.buildDir)
	if err != nil {
		return err
	}
	return execute(ctx, flags, m)
}

type buildModules interface {
	Images() []build.Image
	Platforms() []build.DimensionSet
	TestSpecs() []build.TestSpec
	TestListLocation() []string
	TestDurations() []build.TestDuration
	PackageRepositories() []build.PackageRepo
}

func execute(ctx context.Context, flags testsharderFlags, m buildModules) error {
	targetDuration := time.Duration(flags.targetDurationSecs) * time.Second
	if flags.targetTestCount > 0 && targetDuration > 0 {
		return fmt.Errorf("max-shard-size and target-duration-secs cannot both be set")
	}

	perTestTimeout := time.Duration(flags.perTestTimeoutSecs) * time.Second

	if err := testsharder.ValidateTests(m.TestSpecs(), m.Platforms()); err != nil {
		return err
	}

	opts := &testsharder.ShardOptions{
		Tags: flags.tags,
	}
	// Pass in the test-list to carry over tags to the shards.
	testListPath := filepath.Join(flags.buildDir, m.TestListLocation()[0])
	testListEntries, err := build.LoadTestList(testListPath)
	if err != nil {
		return err
	}
	shards := testsharder.MakeShards(m.TestSpecs(), testListEntries, opts)

	if perTestTimeout > 0 {
		testsharder.ApplyTestTimeouts(shards, perTestTimeout)
	}

	testDurations := testsharder.NewTestDurationsMap(m.TestDurations())
	shards = testsharder.AddExpectedDurationTags(shards, testDurations)

	var modifiers []testsharder.ModifierMatch
	if flags.modifiersPath != "" {
		modifiers, err = testsharder.LoadTestModifiers(ctx, m.TestSpecs(), flags.modifiersPath)
		if err != nil {
			return err
		}
	}

	if flags.affectedTestsPath != "" {
		affectedModifiers, err := testsharder.AffectedModifiers(m.TestSpecs(), flags.affectedTestsPath, flags.affectedTestsMaxAttempts, flags.affectedTestsMultiplyThreshold)
		if err != nil {
			return err
		}
		if len(affectedModifiers) == 0 {
			// If there are no affected tests, that means we weren't
			// able to determine which tests were affected so we should
			// run all tests.
			flags.skipUnaffected = false
		}
		modifiers = append(modifiers, affectedModifiers...)
	} else {
		// If no affected-tests file was provided, we don't know which tests
		// were affected, so run all tests.
		flags.skipUnaffected = false
	}

	shards, err = testsharder.ApplyModifiers(shards, modifiers)
	if err != nil {
		return err
	}

	// At this point, each test in each shard should be updated to tell
	// whether they are affected and whether they should be multiplied with
	// the max number of runs to run.
	shards = testsharder.SplitOutMultipliers(ctx, shards, testDurations, targetDuration, flags.targetTestCount)

	// Remove the multiplied shards from the set of shards to analyze for
	// affected tests, as we want to run these shards regardless of whether
	// the associated tests are affected.
	var multipliedShards []*testsharder.Shard
	var nonMultipliedShards []*testsharder.Shard
	for _, shard := range shards {
		if strings.HasPrefix(shard.Name, testsharder.MultipliedShardPrefix) {
			multipliedShards = append(multipliedShards, shard)
		} else {
			nonMultipliedShards = append(nonMultipliedShards, shard)
		}
	}

	var skippedShards []*testsharder.Shard
	if flags.affectedOnly {
		affected := func(t testsharder.Test) bool {
			return t.Affected
		}
		affectedShards, _ := testsharder.PartitionShards(nonMultipliedShards, affected, testsharder.AffectedShardPrefix)
		shards = affectedShards
	} else {
		// Filter out the affected, hermetic shards from the non-multiplied shards.
		hermeticAndAffected := func(t testsharder.Test) bool {
			return t.Affected && t.Hermetic()
		}
		affectedHermeticShards, unaffectedOrNonhermeticShards := testsharder.PartitionShards(nonMultipliedShards, hermeticAndAffected, testsharder.AffectedShardPrefix)
		shards = affectedHermeticShards

		// Filter out unaffected hermetic shards from the remaining shards.
		hermetic := func(t testsharder.Test) bool {
			return t.Hermetic()
		}
		unaffectedHermeticShards, nonhermeticShards := testsharder.PartitionShards(unaffectedOrNonhermeticShards, hermetic, testsharder.HermeticShardPrefix)
		if flags.skipUnaffected {
			// Mark the unaffected, hermetic shards skipped, as we don't need to
			// run them.
			skippedShards, err = testsharder.MarkShardsSkipped(unaffectedHermeticShards)
			if err != nil {
				return err
			}
		} else {
			shards = append(shards, unaffectedHermeticShards...)
		}
		// The shards should include:
		// 1. Affected hermetic shards
		// 2. Unaffected hermetic shards (may be skipped)
		// 3. Nonhermetic shards
		shards = append(shards, nonhermeticShards...)
	}
	// Add the multiplied shards back into the list of shards to run.
	shards = append(shards, multipliedShards...)

	shards = testsharder.WithTargetDuration(shards, targetDuration, flags.targetTestCount, flags.maxShardsPerEnvironment, testDurations)

	if flags.imageDeps || flags.hermeticDeps {
		for _, s := range shards {
			if err := testsharder.AddImageDeps(s, flags.buildDir, m.Images(), flags.pave); err != nil {
				return err
			}
		}
	}

	if flags.perShardPackageRepos || flags.hermeticDeps {
		pkgRepos := m.PackageRepositories()
		if len(pkgRepos) < 1 {
			return errors.New("build did not generate a package repository")
		}
		absPkgRepoPath := filepath.Join(flags.buildDir, pkgRepos[0].Path)
		if _, err := os.Stat(absPkgRepoPath); errors.Is(err, os.ErrNotExist) {
			logger.Warningf(ctx, "package repository %s does not exist, not creating per-shard package repos", absPkgRepoPath)
		} else if err != nil {
			return err
		} else {
			for _, s := range shards {
				if err := s.CreatePackageRepo(flags.buildDir, pkgRepos[0].Path, flags.cacheTestPackages || flags.hermeticDeps); err != nil {
					return err
				}
			}
		}
	}

	if err := testsharder.ExtractDeps(shards, flags.buildDir); err != nil {
		return err
	}

	if flags.realmLabel != "" {
		testsharder.ApplyRealmLabel(shards, flags.realmLabel)
	}

	// Add back the skipped shards so that we can process and upload results
	// downstream.
	shards = append(shards, skippedShards...)

	f := os.Stdout
	if flags.outputFile != "" {
		var err error
		f, err = os.Create(flags.outputFile)
		if err != nil {
			return fmt.Errorf("unable to create %s: %v", flags.outputFile, err)
		}
		defer f.Close()
	}

	encoder := json.NewEncoder(f)
	// Use 4-space indents so golden files are compatible with `fx format-code`.
	encoder.SetIndent("", "    ")
	if err := encoder.Encode(&shards); err != nil {
		return fmt.Errorf("failed to encode shards: %v", err)
	}
	return nil
}
