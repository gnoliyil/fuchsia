// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package file

import (
	"fmt"
	"hash/fnv"
	"os"
	"path/filepath"
)

// File is a data struct used to hold the path and text content
// of a file in the source tree.
type File struct {
	name        string
	absPath     string
	relPath     string
	url         string
	projectName string
	fileType    FileType

	contentLoaded bool
	data          []*FileData
	text          []byte

	// SPDX fields for referencing this file content
	// in the SPDX output file.
	spdxName string
	spdxID   string
}

// Order implements sort.Interface for []*File based on the AbsPath field.
type Order []*File

func (a Order) Len() int           { return len(a) }
func (a Order) Swap(i, j int)      { a[i], a[j] = a[j], a[i] }
func (a Order) Less(i, j int) bool { return a[i].absPath < a[j].absPath }

// LoadFile returns a new File struct, with the file content loaded in.
func LoadFile(path string, ft FileType, projectName string) (*File, error) {
	var err error

	// If this file was already created, return the previous File object.
	if f, ok := AllFiles[path]; ok {
		plusVal(RepeatedFileTraversal, path)
		return f, nil
	}

	// Verify that the file actually exists
	if _, err := os.Stat(path); err != nil {
		// If the above command fails, this file may be a symbolic link
		if _, err := os.Lstat(path); err != nil {
			// This filepath doesn't exist at all
			return nil, err
		}
	}

	relPath := path
	if filepath.IsAbs(path) {
		if relPath, err = filepath.Rel(Config.FuchsiaDir, path); err != nil {
			return nil, err
		}
	}

	absPath := path
	if absPath, err = filepath.Abs(path); err != nil {
		return nil, err
	}

	plusVal(NumFiles, path)
	if Config.Extensions[filepath.Ext(path)] {
		plusVal(NumPotentialLicenseFiles, path)
	}

	name := filepath.Base(path)
	f := &File{
		name:        name,
		absPath:     absPath,
		relPath:     relPath,
		fileType:    ft,
		projectName: projectName,
		spdxName:    fmt.Sprintf("%s - %s", projectName, name),
	}

	h := fnv.New128a()
	h.Write([]byte(fmt.Sprintf("%s %s", f.projectName, f.RelPath())))
	f.spdxID = fmt.Sprintf("LicenseRef-file-%x", h.Sum([]byte{}))

	AllFiles[path] = f
	return f, nil
}

// Setters
// TODO(fxbug.dev/125736): Remove all setters.
func (f *File) SetURL(url string) { f.url = url }

// Getters
func (f *File) Name() string        { return f.name }
func (f *File) AbsPath() string     { return f.absPath }
func (f *File) RelPath() string     { return f.relPath }
func (f *File) URL() string         { return f.url }
func (f *File) ProjectName() string { return f.projectName }
func (f *File) FileType() FileType  { return f.fileType }
func (f *File) SPDXName() string    { return f.spdxName }
func (f *File) SPDXID() string      { return f.spdxID }
func (f *File) Data() ([]*FileData, error) {
	if err := f.LoadContent(); err != nil {
		return nil, err
	}
	return f.data, nil
}

func (f *File) Text() ([]byte, error) {
	if err := f.LoadContent(); err != nil {
		return nil, err
	}
	return f.text, nil
}

func (f *File) LoadContent() error {
	if f.contentLoaded {
		return nil
	}

	content, err := os.ReadFile(f.absPath)
	if err != nil {
		return err
	}

	// Some source files are extremely large.
	// Only load in the top portion of regular files to save memory.
	if f.fileType == RegularFile && len(content) > 0 {
		content = content[:min(Config.CopyrightSize, len(content))]
	}

	data, err := LoadFileData(f, content)
	if err != nil {
		return err
	}

	f.data = data
	f.text = content
	f.contentLoaded = true
	return nil
}

func (f *File) UnloadContent() {
	f.data = nil
	f.text = nil
	f.contentLoaded = false
}

func (f *File) UpdateURLs(projectName string, projectURL string) {
	for _, d := range f.data {
		d.UpdateURLs(projectName, projectURL)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
