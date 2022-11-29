// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain

package syslog

import (
	"context"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall/zx"

	"fidl/fuchsia/diagnostics"
	"fidl/fuchsia/logger"
)

// ErrMsgTooLong is returned when a message is truncated.
type ErrMsgTooLong struct {
	// Msg is the truncated part of the message.
	Msg string
}

func (e *ErrMsgTooLong) Error() string {
	return "too long message was truncated"
}

const (
	_ uint8 = iota
	DebugVerbosity
	TraceVerbosity
)

type LogLevel int8

const (
	TraceLevel   = LogLevel(logger.LogLevelFilterTrace)
	DebugLevel   = LogLevel(logger.LogLevelFilterDebug)
	InfoLevel    = LogLevel(logger.LogLevelFilterInfo)
	WarningLevel = LogLevel(logger.LogLevelFilterWarn)
	ErrorLevel   = LogLevel(logger.LogLevelFilterError)
	FatalLevel   = LogLevel(logger.LogLevelFilterFatal)
	NoneLevel    = LogLevel(logger.LogLevelFilterNone)
)

var _ flag.Value = (*LogLevel)(nil)

// Set implements the flag.Value interface.
func (ll *LogLevel) Set(s string) error {
	s = strings.ToUpper(s)
	lls := logLevelFromString(s)
	if lls != NoneLevel {
		*ll = lls
		return nil
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return err
	}
	*ll = LogLevel(v)
	return nil
}

func logLevelFromString(s string) LogLevel {
	switch s {
	case "ALL":
		return math.MinInt8
	case "TRACE":
		return TraceLevel
	case "DEBUG":
		return DebugLevel
	case "INFO":
		return InfoLevel
	case "WARNING":
		return WarningLevel
	case "ERROR":
		return ErrorLevel
	case "FATAL":
		return FatalLevel
	default:
		return NoneLevel
	}
}

func (ll LogLevel) String() string {
	switch ll {
	case math.MinInt8:
		return "ALL"
	case TraceLevel:
		return "TRACE"
	case DebugLevel:
		return "DEBUG"
	case InfoLevel:
		return "INFO"
	case WarningLevel:
		return "WARNING"
	case ErrorLevel:
		return "ERROR"
	case FatalLevel:
		return "FATAL"
	default:
		return fmt.Sprintf("LOG(%d)", ll)
	}
}

type Writer struct {
	*Logger
}

func (l *Writer) Write(data []byte) (n int, err error) {
	origLen := len(data)

	// Strip out the trailing newline the `log` library adds because the
	// logging service also adds a trailing newline.
	if len(data) > 0 && data[len(data)-1] == '\n' {
		data = data[:len(data)-1]
	}

	if err := l.Infof("%s", data); err != nil {
		return 0, err
	}

	return origLen, nil
}

type logOptions struct {
	LogSink                       *logger.LogSinkWithCtxInterface
	MinSeverityForFileAndLineInfo LogLevel
	Tags                          []string
	Writer                        io.Writer
}

type LogInitOptions struct {
	logOptions
	LogLevel LogLevel
}

// LogEncoder class, used for encoding structured logs.
type LogEncoder struct {
	// 65-kb buffer to encode to
	buf [65536]byte

	// Current offset into the buffer
	offset int

	// Arg size in bytes
	argSize int

	// Offset of the current arg header
	argHeaderOffset int

	// Offset of the current header
	headerOffset int

	// Unaligned key size in bytes
	unalignedKeySize int

	// Log severity
	severity LogLevel
}

// New method
func NewLogEncoder() *LogEncoder {
	return &LogEncoder{}
}

// Write uint64 method
func (e *LogEncoder) WriteUint64(value uint64) {
	binary.LittleEndian.PutUint64(e.buf[e.offset:], value)
	e.offset += 8
}

// Flush previous argument
func (e *LogEncoder) FlushPreviousArgument() {
	e.argSize = 0
}

// Aligns a byte offset to the next 8-byte (word) boundary
func AlignToWord(value int) int {
	if value%8 == 0 {
		return value
	}
	return (value + 8) &^ 7
}

// Write a string to the buffer
func (e *LogEncoder) WriteString(value string) {
	// Write the string to the buffer
	copy(e.buf[e.offset:], value)
	e.offset += AlignToWord(len(value))
}

func boolToUint64(value bool) uint64 {
	if value {
		return 1
	}
	return 0
}

// Compute argument header
func (e *LogEncoder) ComputeArgumentHeader(argtype uint64) uint64 {
	return argtype | (uint64(e.argSize) << 4) | (uint64(e.unalignedKeySize) << 16) | (boolToUint64(e.unalignedKeySize > 0) << 31)
}

// Append argument key
func (e *LogEncoder) AppendArgumentKey(key string) {
	e.FlushPreviousArgument()
	e.argHeaderOffset = e.offset
	// Write an empty header
	e.WriteUint64(0)
	copy(e.buf[e.offset:], key)
	var aligned = AlignToWord(len(key))
	e.unalignedKeySize = len(key)
	e.offset += aligned
	e.argSize += aligned + 8 // 8 extra bytes for the header
}

// Append argument uint64 value (type 4)
func (e *LogEncoder) AppendArgumentValueUint64(value uint64) {
	e.WriteUint64(value)

	e.argSize += 8

	// Write the argument header
	binary.LittleEndian.PutUint64(e.buf[e.argHeaderOffset:], e.ComputeArgumentHeader(4))
}

// Append argument int64 value (type 3)
func (e *LogEncoder) AppendArgumentValueInt64(value int64) {
	e.WriteUint64(uint64(value))
	e.argSize += 8

	binary.LittleEndian.PutUint64(e.buf[e.argHeaderOffset:], e.ComputeArgumentHeader(3))
}

// Append argument string value (type 6)
func (e *LogEncoder) AppendArgumentValueString(value string) {
	copy(e.buf[e.offset:], value)
	var aligned = AlignToWord(len(value))
	e.offset += aligned
	e.argSize += aligned

	binary.LittleEndian.PutUint64(e.buf[e.argHeaderOffset:], e.ComputeArgumentHeader(6)|(((boolToUint64(len(value) > 0)<<15)|uint64(len(value)))<<32))
}

func (e *LogEncoder) Begin(timestamp uint64, severity LogLevel, file *string, line *uint32, pid uint64, tid uint64, msg string, tags []string) {
	e.offset = 0
	e.argSize = 0
	e.buf = [65536]byte{}
	e.severity = severity

	e.headerOffset = e.offset

	// Write an empty header.
	// This is initially empty to reserve space until we know
	// the full value of the header.
	e.WriteUint64(0)

	e.WriteUint64(timestamp)

	e.AppendArgumentKey("pid")
	e.AppendArgumentValueUint64(pid)

	e.AppendArgumentKey("tid")
	e.AppendArgumentValueUint64(tid)

	e.AppendArgumentKey("message")
	e.AppendArgumentValueString(msg)

	if file != nil {
		e.AppendArgumentKey("file")
		e.AppendArgumentValueString(*file)
	}

	if line != nil {
		e.AppendArgumentKey("line")
		e.AppendArgumentValueUint64(uint64(*line))
	}

	for _, tag := range tags {
		e.AppendArgumentKey("tag")
		e.AppendArgumentValueString(tag)
	}
}

// Gets the buffer up to the offset that was written
func (e *LogEncoder) GetBuffer() []byte {
	return e.buf[:e.offset]
}

func (e *LogEncoder) End() {
	e.FlushPreviousArgument()
	var recordSizeInWords = (e.offset - e.headerOffset) / 8

	// Write the header
	binary.LittleEndian.PutUint64(e.buf[e.headerOffset:], 9|(uint64(recordSizeInWords)<<4)|(uint64(e.severity)<<56))
}

type Logger struct {
	socket zx.Socket
	cancel context.CancelFunc

	droppedLogs uint32
	options     logOptions
	level       int32 // LogLevel; accessed atomically.
	pid         uint64
	file        *string
	line        *uint32
}

func NewLogger(options LogInitOptions) (*Logger, error) {
	if l, max := len(options.Tags), int(logger.MaxTags); l > max {
		return nil, fmt.Errorf("too many tags: %d/%d", l, max)
	}
	for _, tag := range options.Tags {
		if l, max := len(tag), int(logger.MaxTagLenBytes); l > max {
			return nil, fmt.Errorf("tag too long: %d/%d", l, max)
		}
	}
	l := Logger{
		options: options.logOptions,
		level:   int32(options.LogLevel),
		pid:     uint64(os.Getpid()),
	}
	if logSink := options.LogSink; logSink != nil {
		const tag = "syslog"
		ctx, cancel := context.WithCancel(context.Background())
		l.cancel = func() {
			cancel()
			if err := logSink.Close(); err != nil {
				_ = l.WarnTf(tag, "fuchsia.logger/LogSink.Close(): %s", err)
			}
		}

		localS, peerS, err := zx.NewSocket(zx.SocketDatagram)
		if err != nil {
			_ = l.Close()
			return nil, err
		}
		l.socket = localS
		if err := logSink.ConnectStructured(context.Background(), peerS); err != nil {
			_ = l.Close()
			return nil, err
		}

		initLevel := options.LogLevel
		go func() {
			for {
				_ = l.InfoTf(tag, "calling fuchsia.logger/LogSink.WaitForInterestChange...")
				interest, err := logSink.WaitForInterestChange(ctx)
				if err != nil {
					func() {
						switch err := err.(type) {
						case *zx.Error:
							switch err.Status {
							case zx.ErrBadHandle, zx.ErrCanceled, zx.ErrPeerClosed:
								return
							}
							_ = l.WarnTf(tag, "fuchsia.logger/LogSink.WaitForInterestChange(): %s", err)
						}
					}()
					return
				}
				switch interest.Which() {
				case logger.LogSinkWaitForInterestChangeResultResponse:
					if interest.Response.Data.HasMinSeverity() {
						minSeverity := interest.Response.Data.GetMinSeverity()
						l.SetSeverity(minSeverity)
						_ = l.InfoTf(tag, "fuchsia.logger/LogSink.WaitForInterestChange({MinSeverity: %s})", minSeverity)
					} else {
						_ = l.InfoTf(tag, "fuchsia.logger/LogSink.WaitForInterestChange({})")
						l.SetSeverity(diagnostics.Severity(initLevel))
					}
				case logger.LogSinkWaitForInterestChangeResultErr:
					_ = l.ErrorTf(tag, "fuchsia.logger/LogSink.WaitForInterestChange(): %s", interest.Err)
					return
				}
			}
		}()
	}
	return &l, nil
}

func NewLoggerWithDefaults(tags ...string) (*Logger, error) {
	options := LogInitOptions{
		LogLevel: InfoLevel,
	}
	options.MinSeverityForFileAndLineInfo = ErrorLevel
	options.Tags = tags
	return NewLogger(options)
}

func (l *Logger) Close() error {
	l.cancel()
	return l.socket.Close()
}

func (l *Logger) logToWriter(writer io.Writer, time zx.Time, logLevel LogLevel, tag, msg string) error {
	if writer == nil {
		writer = os.Stderr
	}
	var tagsStorage [logger.MaxTags + 1]string
	tags := tagsStorage[:0]
	for _, tag := range l.options.Tags {
		if len(tag) > 0 {
			tags = append(tags, tag)
		}
	}
	if len(tag) > 0 {
		tags = append(tags, tag)
	}
	var buffer [int(logger.MaxTags+1) * int(logger.MaxTagLenBytes)]byte
	pos := 0
	for i, tag := range tags {
		if i > 0 {
			pos += copy(buffer[pos:], ", ")
		}
		pos += copy(buffer[pos:], tag)
	}
	_, err := fmt.Fprintf(writer, "[%05d.%06d][%d][0][%s] %s: %s\n", time/1000000000, (time/1000)%1000000, l.pid, buffer[:pos], logLevel, msg)
	return err
}

func (l *Logger) logToSocket(time zx.Time, logLevel LogLevel, tag, msg string) error {
	const golangThreadID = 0

	var encoder = NewLogEncoder()
	encoder.Begin(uint64(time), logLevel, l.file, l.line, l.pid, golangThreadID, msg, append(l.options.Tags, tag))
	l.file = nil
	l.line = nil
	encoder.End()
	if _, err := l.socket.Write(encoder.GetBuffer(), 0); err != nil {
		return err
	}
	return nil

}

func (l *Logger) logf(callDepth int, logLevel LogLevel, tag string, format string, a ...interface{}) error {
	if LogLevel(atomic.LoadInt32(&l.level)) > logLevel {
		return nil
	}
	time := zx.Sys_clock_get_monotonic()
	msg := fmt.Sprintf(format, a...)
	if logLevel >= l.options.MinSeverityForFileAndLineInfo {
		_, file, line, ok := runtime.Caller(callDepth)
		if !ok {
			file = "???"
			line = 0
		} else {
			short := file
			for i := len(file) - 1; i > 0; i-- {
				if file[i] == '/' {
					short = file[i+1:]
					break
				}
			}
			file = short
		}
		if atomic.LoadUint32((*uint32)(&l.socket)) == uint32(zx.HandleInvalid) {
			msg = fmt.Sprintf("%s(%d): %s ", file, line, msg)
		} else {
			l.file = &file
			var lineHeapAllocated = uint32(line)
			l.line = &lineHeapAllocated
		}
	}
	if len(tag) > int(logger.MaxTagLenBytes) {
		tag = tag[:logger.MaxTagLenBytes]
	}
	if logLevel == FatalLevel {
		defer os.Exit(1)
	}
	if atomic.LoadUint32((*uint32)(&l.socket)) != uint32(zx.HandleInvalid) {
		switch err := l.logToSocket(time, logLevel, tag, msg).(type) {
		case *zx.Error:
			switch err.Status {
			case zx.ErrPeerClosed, zx.ErrBadState:
				atomic.StoreUint32((*uint32)(&l.socket), uint32(zx.HandleInvalid))
			default:
				return err
			}
		default:
			return err
		}
	}
	return l.logToWriter(l.options.Writer, time, logLevel, tag, msg)
}

func (l *Logger) SetSeverity(severity diagnostics.Severity) {
	atomic.StoreInt32(&l.level, int32(severity))
}

// severityFromVerbosity provides the severity corresponding to the given
// verbosity. Note that verbosity relative to the default severity and can
// be thought of as incrementally "more vebose than" the baseline.
func severityFromVerbosity(verbosity uint8) LogLevel {
	level := InfoLevel - LogLevel(verbosity*logger.LogVerbosityStepSize)
	// verbosity scale sits in the interstitial space between INFO and DEBUG
	if level < (DebugLevel + 1) {
		return DebugLevel + 1
	}
	return level
}

func (l *Logger) SetVerbosity(verbosity uint8) {
	atomic.StoreInt32(&l.level, int32(severityFromVerbosity(verbosity)))
}

const DefaultCallDepth = 2

func (l *Logger) Tracef(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, TraceLevel, "", format, a...)
}

func (l *Logger) Debugf(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, DebugLevel, "", format, a...)
}

func (l *Logger) Infof(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, InfoLevel, "", format, a...)
}

func (l *Logger) Warnf(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, WarningLevel, "", format, a...)
}

func (l *Logger) Errorf(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, ErrorLevel, "", format, a...)
}

func (l *Logger) Fatalf(format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, FatalLevel, "", format, a...)
}

func (l *Logger) VLogf(verbosity uint8, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, severityFromVerbosity(verbosity), "", format, a...)
}

func (l *Logger) TraceTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, TraceLevel, tag, format, a...)
}

func (l *Logger) DebugTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, DebugLevel, tag, format, a...)
}

func (l *Logger) InfoTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, InfoLevel, tag, format, a...)
}

func (l *Logger) WarnTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, WarningLevel, tag, format, a...)
}

func (l *Logger) ErrorTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, ErrorLevel, tag, format, a...)
}

func (l *Logger) FatalTf(tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, FatalLevel, tag, format, a...)
}

func (l *Logger) VLogTf(verbosity uint8, tag, format string, a ...interface{}) error {
	return l.logf(DefaultCallDepth, severityFromVerbosity(verbosity), tag, format, a...)
}

var defaultLogger = &Logger{
	level: int32(InfoLevel),
	options: logOptions{
		MinSeverityForFileAndLineInfo: ErrorLevel,
		Writer:                        os.Stderr,
	},
	pid: uint64(os.Getpid()),
}

func Logf(callDepth int, logLevel LogLevel, tag string, format string, a ...interface{}) error {
	if l := GetDefaultLogger(); l != nil {
		return l.logf(callDepth+1, logLevel, tag, format, a...)
	}
	return errors.New("default logger not initialized")
}

func GetDefaultLogger() *Logger {
	return defaultLogger
}

func SetDefaultLogger(l *Logger) {
	defaultLogger = l
}

func SetSeverity(logLevel LogLevel) {
	if l := GetDefaultLogger(); l != nil {
		atomic.StoreInt32(&l.level, int32(logLevel))
	}
}

func SetVerbosity(verbosity uint8) {
	SetSeverity(severityFromVerbosity(verbosity))
}

func Tracef(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, TraceLevel, "", format, a...)
}

func Debugf(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, DebugLevel, "", format, a...)
}

func Infof(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, InfoLevel, "", format, a...)
}

func Warnf(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, WarningLevel, "", format, a...)
}

func Errorf(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, ErrorLevel, "", format, a...)
}

func Fatalf(format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, FatalLevel, "", format, a...)
}

func VLogf(verbosity uint8, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, severityFromVerbosity(verbosity), "", format, a...)
}

func TraceTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, TraceLevel, tag, format, a...)
}

func DebugTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, DebugLevel, tag, format, a...)
}

func InfoTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, InfoLevel, tag, format, a...)
}

func WarnTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, WarningLevel, tag, format, a...)
}

func ErrorTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, ErrorLevel, tag, format, a...)
}

func FatalTf(tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, FatalLevel, tag, format, a...)
}

func VLogTf(verbosity uint8, tag, format string, a ...interface{}) error {
	return Logf(DefaultCallDepth, severityFromVerbosity(verbosity), tag, format, a...)
}
