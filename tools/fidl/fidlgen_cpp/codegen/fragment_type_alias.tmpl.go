// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentTypeAliasTmpl = `
{{- define "NaturalTypeAlias" }}
{{ EnsureNamespace . }}

{{- .Docs }}
using {{ .Name }} = {{ .Natural }};

{{- /* Natural types strict bits is a simple enum class, hence the mask is
       stored on-the-side in the form of an extra constant. */}}
{{- if Eq .Kind Kinds.Bits }}
{{- if .IsStrict }}
const static {{ .Name }} {{ .MaskName.Name }} = {{ .MaskName.Natural }};
{{- end }}
{{- end }}
{{ end }}
`
