// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !tracing

package provider

import (
	"go.fuchsia.dev/fuchsia/src/lib/component"
	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"
)

const tag = "trace-provider"

func Create(*component.Context) error {
	_ = syslog.InfoTf(tag, "tracing is disabled")
	return nil
}
