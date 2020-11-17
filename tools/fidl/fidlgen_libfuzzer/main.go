// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"flag"
	"log"
	"os"

	"go.fuchsia.dev/fuchsia/tools/fidl/fidlgen_libfuzzer/codegen"
	fidl "go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var jsonPath = flag.String("json", "",
	"relative path to the FIDL intermediate representation.")
var outputBase = flag.String("output-base", "",
	"the base file name for files generated by this generator.")
var includeBase = flag.String("include-base", "",
	"the directory to which C and C++ includes should be relative.")
var includeStem = flag.String("include-stem", "cpp/libfuzzer",
	"[optional] the suffix after library path when referencing includes. "+
		"Includes will be of the form <my/library/{include-stem}.h>. ")
var clangFormatPath = flag.String("clang-format-path", "",
	"path to the clang-format tool.")

func flagsValid() bool {
	return *jsonPath != "" && *outputBase != "" && *includeBase != ""
}

func main() {
	flag.Parse()
	if !flag.Parsed() || !flagsValid() {
		flag.PrintDefaults()
		os.Exit(1)
	}

	ir, err := fidl.ReadJSONIr(*jsonPath)
	if err != nil {
		log.Fatal(err)
	}

	config := codegen.Config{
		OutputBase:  *outputBase,
		IncludeBase: *includeBase,
		IncludeStem: *includeStem,
	}
	if err := codegen.NewFidlGenerator().GenerateFidl(ir, &config, *clangFormatPath); err != nil {
		log.Fatalf("Error running generator: %v", err)
	}
}
