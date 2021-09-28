// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fileMarkersTmpl = `
{{- define "Filename:Markers" -}}
fidl/{{ .LibraryDots }}/cpp/markers.h
{{- end }}


{{- define "File:Markers" -}}
  {{- UseWire -}}
  // WARNING: This file is machine generated by fidlgen.

  #pragma once

  {{ range .Decls }}
    {{- if Eq .Kind Kinds.Protocol }}
      {{ EnsureNamespace . }}
      {{ .Docs }}
      class {{ .Name }} final {
        {{ .Name }}() = delete;
      public:
        {{- range .Methods }}
          {{- .Docs }}
          class {{ .Marker.Self }} final {
            {{ .Marker.Self }}() = delete;
          };
        {{- end }}
      };
    {{- end }}
  {{- end }}
  {{ EndOfFile }}
{{ end }}

`
