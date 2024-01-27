// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package build

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"

	versionHistory "go.fuchsia.dev/fuchsia/src/lib/versioning/version-history/go"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/bin/pm/pkg"
	"go.fuchsia.dev/fuchsia/src/sys/pkg/lib/merkle"
)

// Config contains global build configuration for other build commands
type Config struct {
	OutputDir       string
	ManifestPath    string
	KeyPath         string
	TempDir         string
	PkgName         string
	PkgRepository   string
	PkgVersion      string
	PkgABIRevision  uint64
	SubpackagesPath string

	// the manifest is memoized lazily, on the first call to Manifest()
	manifest *Manifest
}

// NewConfig initializes a new configuration with conventional defaults
func NewConfig() *Config {
	cfg := &Config{
		OutputDir:       ".",
		ManifestPath:    ".",
		KeyPath:         "",
		TempDir:         os.TempDir(),
		PkgName:         "",
		PkgRepository:   "",
		PkgVersion:      "0",
		SubpackagesPath: "",
		PkgABIRevision:  0,
	}
	return cfg
}

// TestConfig produces a configuration suitable for testing. It creates a
// temporary directory as a parent of the returned config.OutputDir and
// config.TempDir. Callers should remove this directory.
func TestConfig() *Config {
	d, err := os.MkdirTemp("", "pm-test")
	if err != nil {
		panic(err)
	}
	cfg := &Config{
		OutputDir:       filepath.Join(d, "output"),
		ManifestPath:    filepath.Join(d, "manifest"),
		KeyPath:         filepath.Join(d, "key"),
		TempDir:         filepath.Join(d, "tmp"),
		PkgName:         "testpackage",
		PkgRepository:   "testrepository.com",
		PkgVersion:      "0",
		SubpackagesPath: "",
		PkgABIRevision:  TestABIRevision,
	}
	for _, d := range []string{cfg.OutputDir, cfg.TempDir} {
		os.MkdirAll(d, os.ModePerm)
	}
	return cfg
}

// InitFlags adds flags to a flagset for altering Config defaults
func (c *Config) InitFlags(fs *flag.FlagSet) {
	fs.StringVar(&c.OutputDir, "o", c.OutputDir, "archive output directory")
	fs.StringVar(&c.ManifestPath, "m", c.ManifestPath, "build manifest (or package directory)")
	fs.StringVar(&c.KeyPath, "k", c.KeyPath, "deprecated; do not use")
	fs.StringVar(&c.TempDir, "t", c.TempDir, "temporary directory")
	fs.StringVar(&c.PkgName, "n", c.PkgName, "name of the packages")
	fs.StringVar(&c.PkgRepository, "r", c.PkgRepository, "repository of the packages")
	fs.StringVar(&c.SubpackagesPath, "subpackages", c.SubpackagesPath, "metafile of subpackages")
	fs.Func("api-level", "package API level", func(value string) error {
		if c.PkgABIRevision != 0 {
			return fmt.Errorf("cannot specify both --api-level and --abi-revision")
		}

		apiLevel, err := strconv.ParseUint(value, 0, 64)
		if err != nil {
			return err
		}

		for _, version := range versionHistory.Versions() {
			if version.APILevel == apiLevel {
				c.PkgABIRevision = version.ABIRevision
				return nil
			}
		}

		return fmt.Errorf("API level %d is not defined in the SDK", apiLevel)
	})
	fs.Func("abi-revision", "package ABI revision", func(value string) error {
		if c.PkgABIRevision != 0 {
			return fmt.Errorf("cannot specify both --api-level and --abi-revision")
		}

		abiRevision, err := strconv.ParseUint(value, 0, 64)
		if err != nil {
			return err
		}

		for _, version := range versionHistory.Versions() {
			if version.ABIRevision == abiRevision {
				c.PkgABIRevision = abiRevision
				return nil
			}
		}

		return fmt.Errorf("ABI Revision %d is not defined in the SDK", abiRevision)
	})
}

// Manifest initializes and returns the configured manifest. The manifest may be
// modified during the build process to add/remove files.
func (c *Config) Manifest() (*Manifest, error) {
	var err error
	if c.manifest == nil {
		sources := []string{}

		if c.ManifestPath != "" {
			sources = append(sources, c.ManifestPath)
		}

		// Only use outputdir as a source if no manifest was supplied.
		if c.ManifestPath == "" && c.OutputDir != "" {
			sources = append(sources, c.OutputDir)
		}

		if len(sources) == 0 {
			err = os.ErrNotExist
		}
		c.manifest, err = NewManifest(sources)
	}
	return c.manifest, err
}

// MetaFAR returns the path to the meta.far that build.Seal generates
func (c *Config) MetaFAR() string {
	return filepath.Join(c.OutputDir, "meta.far")
}

func (c *Config) Package() (pkg.Package, error) {
	p := pkg.Package{
		Name:    c.PkgName,
		Version: c.PkgVersion,
	}

	if p.Name == "" {
		p.Name = filepath.Base(c.OutputDir)
		if p.Name == "." {
			var err error
			p.Name, err = filepath.Abs(p.Name)
			if err != nil {
				return p, fmt.Errorf("build: unable to compute package name from directory: %s", err)
			}
			p.Name = filepath.Base(p.Name)
		}
	}

	return p, nil
}

func (c *Config) BlobInfo() ([]PackageBlobInfo, error) {
	manifest, err := c.Manifest()
	if err != nil {
		return nil, err
	}

	var result []PackageBlobInfo

	// Include a meta FAR entry first. If blobs.sizes becomes the new root
	// blob for a package, targets need to know which unnamed blob is the
	// meta FAR.
	{
		archive, err := os.Open(c.MetaFAR())
		if err != nil {
			return nil, err
		}
		var tree merkle.Tree
		if _, err := tree.ReadFrom(archive); err != nil {
			return nil, err
		}
		var merkle MerkleRoot
		copy(merkle[:], tree.Root()[:32])

		info, err := os.Stat(c.MetaFAR())
		if err != nil {
			return nil, err
		}

		result = append(result, PackageBlobInfo{
			SourcePath: c.MetaFAR(),
			Path:       "meta/",
			Merkle:     merkle,
			Size:       uint64(info.Size()),
		})
	}

	contentsPath := filepath.Join(c.OutputDir, "meta", "contents")
	contents, err := LoadMetaContents(contentsPath)
	if err != nil {
		return nil, err
	}

	contentsKeys := make([]string, 0, len(contents))
	for k := range contents {
		contentsKeys = append(contentsKeys, k)
	}
	sort.Strings(contentsKeys)

	for _, path := range contentsKeys {
		merkle := contents[path]
		info, err := os.Stat(manifest.Paths[path])
		if err != nil {
			return nil, err
		}

		result = append(result, PackageBlobInfo{
			SourcePath: manifest.Paths[path],
			Path:       path,
			Merkle:     merkle,
			Size:       uint64(info.Size()),
		})
	}

	return result, nil
}

func (c *Config) OutputManifest() (*PackageManifest, error) {
	p, err := c.Package()
	if err != nil {
		return nil, err
	}
	blobs, err := c.BlobInfo()
	if err != nil {
		return nil, err
	}
	return &PackageManifest{
		Version:    "1",
		Repository: c.PkgRepository,
		Package:    p,
		Blobs:      blobs,
	}, err
}
