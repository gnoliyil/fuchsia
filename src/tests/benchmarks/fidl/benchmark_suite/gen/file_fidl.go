// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"gen/config"
	fidlutil "gen/fidl/util"
	"log"
	"os"
	"path"
	"strings"
	"text/template"
	"time"
)

var fidlTmpl = template.Must(template.New("fidlTmpl").Parse(
	`// Copyright {{ .Year }} The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// GENERATED FILE: Regen with $(fx get-build-dir)/host-tools/regen_fidl_benchmark_suite

library benchmarkfidl;
{{ range .Definitions -}}
{{ .Comment }}
{{ .Body }}
{{ end -}}
`))

type fidlTmplInput struct {
	Year        int
	Definitions []fidlTmplDefinition
}

type fidlTmplDefinition struct {
	Comment  string
	Body     string
	Denylist []config.Binding
}

func genFidlFile(filepath string, fidl config.FidlFile) error {
	var definitions []fidlTmplDefinition
	for _, definition := range fidl.Definitions {
		body, err := fidl.Gen(definition.Config)
		if err != nil {
			return err
		}
		if len(definition.Denylist) != 0 {
			strs := make([]string, len(definition.Denylist))
			for i, binding := range definition.Denylist {
				strs[i] = string(binding)
			}
			attribute := "[BindingsDenylist = \"" + strings.Join(strs, ", ") + "\"]"
			body = attribute + "\n" + body
		}
		definitions = append(definitions, fidlTmplDefinition{
			Body:    formatObj(0, body),
			Comment: formatComment(definition.Comment),
		})
	}

	f, err := os.Create(filepath)
	if err != nil {
		return err
	}
	defer f.Close()
	return fidlTmpl.Execute(f, fidlTmplInput{
		Year:        time.Now().Year(),
		Definitions: definitions,
	})
}

func genFidl(outdir string) {
	for _, fidl := range fidlutil.AllFidlFiles() {
		if !strings.HasSuffix(fidl.Filename, ".gen.test.fidl") {
			log.Fatalf("%s needs .gen.test.fidl suffix", fidl.Filename)
		}
		filepath := path.Join(outdir, fidl.Filename)
		if err := genFidlFile(filepath, fidl); err != nil {
			log.Fatalf("Error generating %s: %s", fidl.Filename, err)
		}
	}
}
