// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package netdevice

import (
	"fmt"
	"math"
	"math/bits"
)

// Entries provides basic logic used in link devices to operate on queues of
// entries that can be in one of three states:
// - queued: entries describing buffers already operated on, queued to be sent
// to the driver (populated buffers for Tx, already processed buffers for Rx).
// - in-flight: entries describing buffers currently owned by the driver, not
// yet returned.
// - ready: entries describing buffers retrieved from the driver, but not yet
// operated on (free unpopulated buffers for Tx, buffers containing inbound
// traffic for Rx).
type Entries struct {
	// sent, queued, readied are indices modulo (capacity << 1). They
	// implement a ring buffer with 3 regions:
	//
	// - sent:queued:    "queued" entries
	// - queued:readied: "ready" entries
	// - readied:sent:   "in-flight" entries
	//
	// boundary conditions:
	// - readied == sent:   all entries are "in-flight" (implies == queued).
	// - sent == queued:    0 queued entries.
	// - queued == readied: 0 ready entries.
	sent, queued, readied uint16
	storage               []uint16
}

// Init initializes entries with a given capacity rounded up to the next power
// of two and limited to 2^15. Returns the adopted capacity. After Init, all the
// entries are in the "in-flight" state.
func (e *Entries) Init(capacity uint16) uint16 {
	if capacity != 0 {
		// Round up to power of 2.
		capacity = 1 << bits.Len16(capacity-1)
	}
	if capacity > (math.MaxUint16>>1)+1 {
		// Limit range to 2^15.
		capacity >>= 1
	}
	*e = Entries{
		storage: make([]uint16, capacity),
	}
	return capacity
}

// mask masks an index to the range [0, capacity).
func (e *Entries) mask(val uint16) uint16 {
	return val & (uint16(len(e.storage)) - 1)
}

// mask2 masks an index to the range [0, capacity*2).
func (e *Entries) mask2(val uint16) uint16 {
	return val & ((uint16(len(e.storage)) << 1) - 1)
}

// IncrementSent marks delta entries as moved from "queued" to "in-flight".
// Delta must be limited to the number of queued entries, otherwise Entries may
// become inconsistent.
func (e *Entries) IncrementSent(delta uint16) {
	e.sent = e.mask2(e.sent + delta)
}

// IncrementQueued marks delta entries as moved from "ready" to "queued".
// Delta must be limited to the number of ready entries, otherwise Entries may
// become inconsistent.
func (e *Entries) IncrementQueued(delta uint16) {
	e.queued = e.mask2(e.queued + delta)
}

// IncrementReadied marks delta entries as moved from "in-flight" to "ready".
// Delta must be limited to the number of in-flight buffers, which is the
// size of the range returned by GetInFlightRange.
func (e *Entries) IncrementReadied(delta uint16) {
	e.readied = e.mask2(e.readied + delta)
}

// HaveQueued returns true if there are entries in the "queued" state.
func (e *Entries) HaveQueued() bool {
	return e.sent != e.queued
}

// HaveReadied returns true if there are entries in the "ready" state.
func (e *Entries) HaveReadied() bool {
	return e.queued != e.readied
}

// InFlight returns the number of buffers entries in flight (owned by the
// driver).
func (e *Entries) InFlight() uint16 {
	if readied, sent := e.getInFlightRange(); readied < sent {
		return sent - readied
	} else {
		return uint16(len(e.storage)) - (readied - sent)
	}
}

// getInFlightRange returns the range of indices for entries in "in-flight"
// state that can be move to readied. The end of the range is always exclusive.
// If range start that is larger than or equal to the range end, it must be
// interpreted as two ranges: (start:) and (:end) as opposed to (start:end).
func (e *Entries) getInFlightRange() (uint16, uint16) {
	if readied, sent := e.mask(e.readied), e.mask(e.sent); readied == sent && e.sent != e.readied {
		return uint16(len(e.storage)), 0
	} else {
		return readied, sent
	}
}

// getQueuedRange returns the range of indices for entries in "queued" state.
// The end of the range is always exclusive. If range start that is larger than
// or equal to the range end, it must be interpreted as two ranges: (start:) and
// (:end) as opposed to (start:end).
func (e *Entries) getQueuedRange() (uint16, uint16) {
	if e.sent == e.queued {
		return uint16(len(e.storage)), 0
	}
	return e.mask(e.sent), e.mask(e.queued)
}

// GetReadied returns the first readied entry. Only valid if HaveReadied is
// true.
func (e *Entries) GetReadied() *uint16 {
	return &e.storage[e.mask(e.queued)]
}

// AddReadied copies the contents of a slice into as many available "in-flight"
// entries as possible, returning the number of copied items.
func (e *Entries) AddReadied(src []uint16) int {
	if readied, sent := e.getInFlightRange(); readied < sent {
		return copy(e.storage[readied:sent], src)
	} else {
		n := copy(e.storage[readied:], src)
		n += copy(e.storage[:sent], src[n:])
		return n
	}
}

// GetQueued copies as many queued entries as possible into the slice, returning
// the number of copied items.
func (e *Entries) GetQueued(dst []uint16) int {
	if sent, queued := e.getQueuedRange(); sent < queued {
		return copy(dst, e.storage[sent:queued])
	} else {
		n := copy(dst, e.storage[sent:])
		n += copy(dst[n:], e.storage[:queued])
		return n
	}
}

// GoString implements fmt.GoStringer.
func (e Entries) GoString() string {
	return fmt.Sprintf("%T{cap=%d, sent=%d, queued=%d, readied=%d}", e, len(e.storage), e.sent, e.queued, e.readied)
}
