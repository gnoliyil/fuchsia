// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain

package syslog_test

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall/zx"
	"syscall/zx/fidl"
	"syscall/zx/zxwait"
	"testing"

	"fidl/fuchsia/diagnostics"
	"fidl/fuchsia/logger"

	"go.fuchsia.dev/fuchsia/src/lib/component"
	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"
)

const format = "integer: %d"

var pid = uint64(os.Getpid())

var _ logger.LogSinkWithCtx = (*logSinkImpl)(nil)

type logSinkImpl struct {
	onConnect             func(fidl.Context, zx.Socket)
	waitForInterestChange <-chan logger.LogSinkWaitForInterestChangeResult
}

func (*logSinkImpl) Connect(fidl.Context, zx.Socket) error {
	return nil
}

func (impl *logSinkImpl) ConnectStructured(ctx fidl.Context, socket zx.Socket) error {
	impl.onConnect(ctx, socket)
	return nil
}

func (impl *logSinkImpl) WaitForInterestChange(fidl.Context) (logger.LogSinkWaitForInterestChangeResult, error) {
	if result := <-impl.waitForInterestChange; result != (logger.LogSinkWaitForInterestChangeResult{}) {
		return result, nil
	}
	return logger.LogSinkWaitForInterestChangeResult{}, &zx.Error{Status: zx.ErrCanceled}
}

type LogDecoder struct {
	// Buffer
	buf []byte

	// Offset
	offset int
}

// New method
func NewLogDecoder(buf []byte) *LogDecoder {
	return &LogDecoder{buf: buf}
}

// Read a uint64 from the buffer
func (d *LogDecoder) ReadUint64() uint64 {
	var value = binary.LittleEndian.Uint64(d.buf[d.offset:])
	d.offset += 8
	return value
}

type LogRecord struct {
	timestamp uint64
	pid       *uint64
	tid       *uint64
	message   *string
	file      *string
	line      *uint64
	tags      []string
	severity  syslog.LogLevel
}

func (d *LogDecoder) ReadRecord() *LogRecord {
	// Record header is unused
	var severity = (d.ReadUint64() >> 56) & 0xff
	var timestamp = d.ReadUint64()

	var record = &LogRecord{}
	record.severity = syslog.LogLevel(severity)
	record.timestamp = timestamp

	for d.offset < len(d.buf) {
		var header = d.ReadUint64()
		// This would contain the argument type if we needed it,
		// but in practice we can rely on the key to determine the related type for
		// go logs (since our Go bindings don't support custom KVPs)

		//var argtype = header & 0xf
		var argSize = (header >> 32) & 0xfff
		var argKeySize = (header >> 16) & 0xfff
		var argKeyPresent = (header >> 31) & 0x1

		if argKeyPresent == 1 {
			var argkey = string(d.buf[d.offset : d.offset+int(argKeySize)])
			d.offset += syslog.AlignToWord(int(argKeySize))
			var argvalue = d.buf[d.offset : d.offset+int(argSize)]

			switch argkey {
			case "pid":
				var value = binary.LittleEndian.Uint64(d.buf[d.offset:])
				record.pid = &value
				d.offset += 8
			case "tid":
				var value = binary.LittleEndian.Uint64(d.buf[d.offset:])
				record.tid = &value
				d.offset += 8
			case "message":
				var value = string(argvalue)
				record.message = &value
			case "file":
				var value = string(argvalue)
				record.file = &value
			case "line":
				var value = binary.LittleEndian.Uint64(d.buf[d.offset:])
				record.line = &value
				d.offset += 8
			case "tag":
				var value = string(argvalue)
				record.tags = append(record.tags, value)
			}
		}
	}

	return record
}

func uint32Ptr(value uint32) *uint32 {
	return &value
}

func uint64Ptr(value uint64) *uint64 {
	return &value
}

func stringPtr(value string) *string {
	return &value
}

func TestLogEncoder(t *testing.T) {
	var encoder = syslog.NewLogEncoder()
	var desiredOutput = LogRecord{
		timestamp: 123456789,
		pid:       uint64Ptr(1234),
		tid:       uint64Ptr(5678),
		message:   stringPtr("Hello world"),
		file:      stringPtr("main.go"),
		line:      uint64Ptr(123),
		tags:      []string{"tag1", "tag2"},
		severity:  syslog.InfoLevel,
	}

	encoder.Begin(desiredOutput.timestamp, syslog.LogLevel(desiredOutput.severity), desiredOutput.file, uint32Ptr(uint32(*desiredOutput.line)), *desiredOutput.pid, *desiredOutput.tid, *desiredOutput.message, desiredOutput.tags)
	encoder.End()

	var decoder = NewLogDecoder(encoder.GetBuffer())
	var output = decoder.ReadRecord()

	if output.timestamp != desiredOutput.timestamp {
		t.Errorf("Timestamp mismatch: %d != %d", output.timestamp, desiredOutput.timestamp)
	}

	if *output.pid != *desiredOutput.pid {
		t.Errorf("PID mismatch: %d != %d", *output.pid, *desiredOutput.pid)
	}

	if *output.tid != *desiredOutput.tid {
		t.Errorf("TID mismatch: %d != %d", *output.tid, *desiredOutput.tid)
	}

	if *output.message != *desiredOutput.message {
		t.Errorf("Message mismatch: %s != %s", *output.message, *desiredOutput.message)
	}

	if *output.file != *desiredOutput.file {
		t.Errorf("File mismatch: %s != %s", *output.file, *desiredOutput.file)
	}

	if *output.line != *desiredOutput.line {
		t.Errorf("Line mismatch: %d != %d", *output.line, *desiredOutput.line)
	}

	if len(output.tags) != len(desiredOutput.tags) {
		t.Errorf("Tag count mismatch: %d != %d", len(output.tags), len(desiredOutput.tags))
	}

	if output.severity != desiredOutput.severity {
		t.Errorf("Severity mismatch %d != %d", output.severity, desiredOutput.severity)
	}

	for i := 0; i < len(output.tags); i++ {
		if output.tags[i] != desiredOutput.tags[i] {
			t.Errorf("Tag mismatch: %s != %s", output.tags[i], desiredOutput.tags[i])
		}
	}

}

func TestLogSimple(t *testing.T) {
	actual := bytes.Buffer{}
	var options syslog.LogInitOptions
	options.MinSeverityForFileAndLineInfo = syslog.ErrorLevel
	options.Writer = &actual
	log, err := syslog.NewLogger(options)
	if err != nil {
		t.Fatal(err)
	}
	if err := log.Infof(format, 10); err != nil {
		t.Fatal(err)
	}
	expected := "INFO: integer: 10\n"
	got := string(actual.Bytes())
	if !strings.HasSuffix(got, expected) {
		t.Errorf("%q should have ended in %q", got, expected)
	}
	if !strings.Contains(got, fmt.Sprintf("[%d]", pid)) {
		t.Errorf("%q should contains %d", got, pid)
	}
}

func setup(t *testing.T, tags ...string) (chan<- logger.LogSinkWaitForInterestChangeResult, zx.Socket, *syslog.Logger) {
	req, logSink, err := logger.NewLogSinkWithCtxInterfaceRequest()
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	ch := make(chan struct{})
	t.Cleanup(func() {
		cancel()
		<-ch
	})

	waitForInterestChange := make(chan logger.LogSinkWaitForInterestChangeResult)
	t.Cleanup(func() { close(waitForInterestChange) })

	sinChan := make(chan zx.Socket, 1)
	defer close(sinChan)
	go func() {
		defer close(ch)

		component.Serve(ctx, &logger.LogSinkWithCtxStub{
			Impl: &logSinkImpl{
				onConnect: func(_ fidl.Context, socket zx.Socket) {
					sinChan <- socket
				},
				waitForInterestChange: waitForInterestChange,
			},
		}, req.Channel, component.ServeOptions{
			OnError: func(err error) {
				var zxError *zx.Error
				if errors.As(err, &zxError) {
					if zxError.Status == zx.ErrCanceled {
						return
					}
				}
				t.Error(err)
			},
		})
	}()

	options := syslog.LogInitOptions{
		LogLevel: syslog.InfoLevel,
	}
	options.LogSink = logSink
	options.MinSeverityForFileAndLineInfo = syslog.ErrorLevel
	options.Tags = tags
	log, err := syslog.NewLogger(options)
	if err != nil {
		t.Fatal(err)
	}

	s := <-sinChan

	// Throw away system-generated messages.
	for i := 0; i < 1; i++ {
		if _, err := zxwait.WaitContext(context.Background(), zx.Handle(s), zx.SignalSocketReadable); err != nil {
			t.Fatal(err)
		}
		var data [logger.MaxDatagramLenBytes]byte
		if _, err := s.Read(data[:], 0); err != nil {
			t.Fatal(err)
		}
	}

	return waitForInterestChange, s, log
}

func checkoutput(t *testing.T, sin zx.Socket, expectedMsg string, severity syslog.LogLevel, tags ...string) {
	var data [logger.MaxDatagramLenBytes]byte
	n, err := sin.Read(data[:], 0)
	if err != nil {
		t.Fatal(err)
	}
	if n <= 32 {
		t.Fatalf("got invalid data: %x", data[:n])
	}
	var msg = NewLogDecoder(data[:]).ReadRecord()
	gotpid := *msg.pid
	gotTid := *msg.tid
	gotTime := msg.timestamp
	gotSeverity := int32(msg.severity)
	gotDroppedLogs := int32(0)

	if pid != gotpid {
		t.Errorf("pid error, got: %d, want: %d", gotpid, pid)
	}

	if 0 != gotTid {
		t.Errorf("tid error, got: %d, want: %d", gotTid, 0)
	}

	if int32(severity) != gotSeverity {
		t.Errorf("severity error, got: %d, want: %d", gotSeverity, severity)
	}

	if gotTime <= 0 {
		t.Errorf("time %d should be greater than zero", gotTime)
	}

	if 0 != gotDroppedLogs {
		t.Errorf("dropped logs error, got: %d, want: %d", gotDroppedLogs, 0)
	}
	for i, tag := range tags {
		var gotTag = msg.tags[i]
		if tag != gotTag {
			t.Fatalf("tag iteration %d: expected tag %q , got %q", i, tag, gotTag)
		}
	}

	msgGot := *msg.message
	if expectedMsg != msgGot {
		t.Fatalf("expected msg:%q, got %q", expectedMsg, msgGot)
	}
}

func TestLog(t *testing.T) {
	_, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()

	if err := log.Infof(format, 10); err != nil {
		t.Fatal(err)
	}
	checkoutput(t, sin, fmt.Sprintf(format, 10), syslog.InfoLevel)
}

func TestLogWithLocalTag(t *testing.T) {
	_, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()
	if err := log.InfoTf("local_tag", format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.InfoLevel, "local_tag")
}

func TestLogWithGlobalTags(t *testing.T) {
	_, sin, log := setup(t, "gtag1", "gtag2")
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()
	if err := log.InfoTf("local_tag", format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.InfoLevel, "gtag1", "gtag2", "local_tag")
}

func TestLoggerSeverity(t *testing.T) {
	_, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()
	log.SetSeverity(diagnostics.Severity(syslog.WarningLevel))
	if err := log.Infof(format, 10); err != nil {
		t.Fatal(err)
	}
	_, err := sin.Read(nil, 0)
	if err, ok := err.(*zx.Error); !ok || err.Status != zx.ErrShouldWait {
		t.Fatal(err)
	}
	if err := log.Warnf(format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.WarningLevel)
}

func TestLoggerVerbosity(t *testing.T) {
	_, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()
	if err := log.VLogf(syslog.DebugVerbosity, format, 10); err != nil {
		t.Fatal(err)
	}
	_, err := sin.Read(nil, 0)
	if err, ok := err.(*zx.Error); !ok || err.Status != zx.ErrShouldWait {
		t.Fatal(err)
	}
	log.SetVerbosity(syslog.DebugVerbosity)
	if err := log.VLogf(syslog.DebugVerbosity, format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.InfoLevel-1)
}

func TestLoggerRegisterInterest(t *testing.T) {
	ch, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()

	registerInterest := func(interest diagnostics.Interest) {
		t.Helper()

		ch <- logger.LogSinkWaitForInterestChangeResultWithResponse(logger.LogSinkWaitForInterestChangeResponse{
			Data: interest,
		})

		// Consume the system-generated messages.
		for i := 0; i < 2; i++ {
			if _, err := zxwait.WaitContext(context.Background(), zx.Handle(sin), zx.SignalSocketReadable); err != nil {
				t.Fatal(err)
			}
			var data [logger.MaxDatagramLenBytes]byte
			if _, err := sin.Read(data[:], 0); err != nil {
				t.Fatal(err)
			}
		}
	}

	// Register interest and observe that the log is emitted.
	{
		var interest diagnostics.Interest
		interest.SetMinSeverity(diagnostics.SeverityDebug)
		registerInterest(interest)
	}
	if err := log.VLogf(syslog.DebugVerbosity, format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.InfoLevel-1)

	// Register empty interest and observe that severity resets to initial.
	registerInterest(diagnostics.Interest{})
	if err := log.VLogf(syslog.DebugVerbosity, format, 10); err != nil {
		t.Fatal(err)
	}
	_, err := sin.Read(nil, 0)
	if err, ok := err.(*zx.Error); !ok || err.Status != zx.ErrShouldWait {
		t.Fatal(err)
	}
}

func TestGlobalTagLimits(t *testing.T) {
	var options syslog.LogInitOptions
	options.Writer = os.Stdout
	var tags [logger.MaxTags + 1]string
	for i := 0; i < len(tags); i++ {
		tags[i] = "a"
	}
	options.Tags = tags[:]
	if _, err := syslog.NewLogger(options); err == nil || !strings.Contains(err.Error(), "too many tags") {
		t.Fatalf("unexpected error: %s", err)
	}
	options.Tags = tags[:logger.MaxTags]
	var tag [logger.MaxTagLenBytes + 1]byte
	for i := 0; i < len(tag); i++ {
		tag[i] = 65
	}
	options.Tags[1] = string(tag[:])
	if _, err := syslog.NewLogger(options); err == nil || !strings.Contains(err.Error(), "tag too long") {
		t.Fatalf("unexpected error: %s", err)
	}
}

func TestLocalTagLimits(t *testing.T) {
	_, sin, log := setup(t)
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
		if err := sin.Close(); err != nil {
			t.Error(err)
		}
	}()
	var tag [logger.MaxTagLenBytes + 1]byte
	for i := 0; i < len(tag); i++ {
		tag[i] = 65
	}
	if err := log.InfoTf(string(tag[:]), format, 10); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf(format, 10)
	checkoutput(t, sin, expectedMsg, syslog.InfoLevel, string(tag[:logger.MaxTagLenBytes]))
}

func TestLogToWriterWhenSocketCloses(t *testing.T) {
	_, sin, log := setup(t, "gtag1", "gtag2")
	defer func() {
		if err := log.Close(); err != nil {
			t.Error(err)
		}
	}()
	if err := sin.Close(); err != nil {
		t.Fatal(err)
	}
	old := os.Stderr
	defer func() {
		os.Stderr = old
	}()

	f, err := os.Create(filepath.Join(t.TempDir(), "syslog-test"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := f.Close(); err != nil {
			t.Error(err)
		}
	}()
	os.Stderr = f
	if err := log.InfoTf("local_tag", format, 10); err != nil {
		t.Fatal(err)
	}
	if err := f.Sync(); err != nil {
		t.Fatal(err)
	}
	expectedMsg := fmt.Sprintf("[0][gtag1, gtag2, local_tag] INFO: %s\n", fmt.Sprintf(format, 10))
	content, err := os.ReadFile(f.Name())
	os.Stderr = old
	if err != nil {
		t.Fatal(err)
	}
	got := string(content)
	if !strings.HasSuffix(got, expectedMsg) {
		t.Fatalf("%q should have ended in %q", got, expectedMsg)
	}
}
