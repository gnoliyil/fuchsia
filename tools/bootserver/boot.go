// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package bootserver

import (
	"bytes"
	"compress/flate"
	"compress/gzip"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"golang.org/x/sync/errgroup"

	constants "go.fuchsia.dev/fuchsia/tools/bootserver/bootserverconstants"
	botanistconstants "go.fuchsia.dev/fuchsia/tools/botanist/constants"
	"go.fuchsia.dev/fuchsia/tools/lib/iomisc"
	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"go.fuchsia.dev/fuchsia/tools/lib/retry"
	"go.fuchsia.dev/fuchsia/tools/net/netboot"
	"go.fuchsia.dev/fuchsia/tools/net/tftp"
)

// Maximum number of times to attempt to download an image.
const maxDownloadAttempts = 3

// Delay between consecutive image download retries.
// Can be overridden in tests to avoid sleeping.
var downloadRetrySleep = 5 * time.Second

// Maps bootserver argument to a corresponding netsvc name.
var bootserverArgToNameMap = map[string]string{
	"--boot":       constants.KernelNetsvcName,
	"--bootloader": constants.BootloaderNetsvcName,
	"--efi":        constants.EfiNetsvcName,
	"--firmware":   constants.FirmwareNetsvcPrefix,
	"--fvm":        constants.FvmNetsvcName,
	"--fxfs":       constants.FxfsNetsvcName,
	"--kernc":      constants.KerncNetsvcName,
	"--vbmetaa":    constants.VbmetaANetsvcName,
	"--vbmetab":    constants.VbmetaBNetsvcName,
	"--vbmetar":    constants.VbmetaRNetsvcName,
	"--zircona":    constants.ZirconANetsvcName,
	"--zirconb":    constants.ZirconBNetsvcName,
	"--zirconr":    constants.ZirconRNetsvcName,
}

func bootserverArgToName(arg string) (string, bool) {
	// Check for a typed firmware arg "--firmare-<type>".
	if strings.HasPrefix(arg, "--firmware-") {
		fwType := strings.TrimPrefix(arg, "--firmware-")
		return constants.FirmwareNetsvcPrefix + fwType, true
	}

	name, exists := bootserverArgToNameMap[arg]
	return name, exists
}

// Maps netsvc name to the index at which the corresponding file should be transferred if
// present. The indices correspond to the ordering given in
// https://go.fuchsia.dev/zircon/+/HEAD/system/host/bootserver/bootserver.c
var transferOrderMap = map[string]int{
	constants.CmdlineNetsvcName:        1,
	constants.FvmNetsvcName:            2,
	constants.FxfsNetsvcName:           3,
	constants.BootloaderNetsvcName:     4,
	constants.FirmwareNetsvcPrefix:     5,
	constants.EfiNetsvcName:            6,
	constants.KerncNetsvcName:          7,
	constants.ZirconANetsvcName:        8,
	constants.ZirconBNetsvcName:        9,
	constants.ZirconRNetsvcName:        10,
	constants.VbmetaANetsvcName:        11,
	constants.VbmetaBNetsvcName:        12,
	constants.VbmetaRNetsvcName:        13,
	constants.AuthorizedKeysNetsvcName: 14,
	constants.KernelNetsvcName:         15,
}

func transferOrder(name string) (int, bool) {
	// Ordering doesn't matter among different types of firmware, they can
	// all use the same index.
	if strings.HasPrefix(name, constants.FirmwareNetsvcPrefix) {
		name = constants.FirmwareNetsvcPrefix
	}

	index, exists := transferOrderMap[name]
	return index, exists
}

// Returns true if this image is OK to skip on error rather than failing out.
// Sometimes this is necessary in order to be able to write to an older netsvc
// which may not know about newer image types.
func skipOnTransferError(name string) bool {
	return strings.HasPrefix(name, constants.FirmwareNetsvcPrefix)
}

// DownloadWithRetries runs a function that downloads a blob from GCS, and
// retries if the function returns an error that corresponds to a failure mode
// that's known to be transient.
func DownloadWithRetries(ctx context.Context, dest string, download func() error) error {
	retryStrategy := retry.WithMaxAttempts(
		retry.NewConstantBackoff(downloadRetrySleep),
		maxDownloadAttempts)
	return retry.Retry(ctx, retryStrategy, func() error {
		if downloadErr := download(); downloadErr != nil {
			// Clean up the destination file, which may be only partially
			// written, before potentially retrying.
			if err := os.RemoveAll(dest); err != nil {
				return retry.Fatal(err)
			}
			if !isTransientDownloadError(ctx, downloadErr) {
				// Don't retry if there was a non-transient failure.
				return retry.Fatal(downloadErr)
			}
			return downloadErr
		}
		return nil
	}, nil)
}

// isTransientDownloadError returns whether an error corresponds to a GCS
// download failure mode that's known to be transient.
func isTransientDownloadError(ctx context.Context, err error) bool {
	if err == nil {
		return false
	}
	if strings.Contains(err.Error(), constants.BadCRCErrorMsg) || errors.Is(err, gzip.ErrChecksum) {
		// Checksum failure likely indicates transient corruption during download.
		logger.Warningf(ctx, "GCS checksum failure detected, retrying download...")
		return true
	}
	var flateErr flate.CorruptInputError
	if errors.As(err, &flateErr) {
		// Ditto, likely indicates transient corruption.
		logger.Warningf(ctx, "Flate corruption error detected, retrying download...")
		return true
	}
	return false
}

func downloadImagesToDir(ctx context.Context, dir string, imgs []Image) ([]Image, func() error, error) {
	// Copy each in a goroutine for efficiency's sake.
	eg := errgroup.Group{}
	mux := sync.Mutex{}
	var newImgs []Image
	for _, img := range imgs {
		if len(img.Args) == 0 || img.Reader == nil {
			continue
		}
		img := img
		eg.Go(func() error {
			var f *os.File
			dest := filepath.Join(dir, img.Name)
			if err := DownloadWithRetries(ctx, dest, func() error {
				var err error
				f, err = downloadAndOpenImage(ctx, dest, img)
				return err
			}); err != nil {
				return err
			}

			fi, err := f.Stat()
			if err != nil {
				f.Close()
				return err
			}
			mux.Lock()
			newImg := img
			newImg.Reader = f
			newImg.Size = fi.Size()
			newImgs = append(newImgs, newImg)
			mux.Unlock()
			return nil
		})
	}

	if err := eg.Wait(); err != nil {
		closeImages(newImgs)
		return nil, noOpClose, err
	}
	return newImgs, func() error { return closeImages(newImgs) }, nil
}

func downloadAndOpenImage(ctx context.Context, dest string, img Image) (*os.File, error) {
	f, ok := img.Reader.(*os.File)
	if ok {
		return f, nil
	}

	// If the file already exists at dest, just open and return the file instead of downloading again.
	// This will avoid duplicate downloads from catalyst (which calls the bootserver tool) and botanist.
	if _, err := os.Stat(dest); !os.IsNotExist(err) {
		return os.Open(dest)
	}

	f, err := os.Create(dest)
	if err != nil {
		return nil, err
	}

	// Log progress to avoid hitting I/O timeout in case of slow transfers.
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	go func() {
		for range ticker.C {
			logger.Debugf(ctx, "transferring %s...", filepath.Base(dest))
		}
	}()

	if _, err := io.Copy(f, iomisc.ReaderAtToReader(img.Reader)); err != nil {
		f.Close()
		return nil, fmt.Errorf("%s (%q to %q): %w", botanistconstants.FailedToCopyImageMsg, img.Name, dest, err)
	}
	if err := f.Close(); err != nil {
		return nil, err
	}
	return os.Open(dest)
}

// Prepares and transfers images to the given client.
// Returns whether the images contained a RAM kernel or not, or error on failure.
func transferImages(ctx context.Context, t tftp.Client, imgs []Image, cmdlineArgs []string, authorizedKeys []byte) (bool, error) {
	var files []*netsvcFile
	if len(cmdlineArgs) > 0 {
		var buf bytes.Buffer
		for _, arg := range cmdlineArgs {
			fmt.Fprintf(&buf, "%s\n", arg)
		}
		reader := bytes.NewReader(buf.Bytes())
		cmdlineFile, err := newNetsvcFile(constants.CmdlineNetsvcName, reader, reader.Size())
		if err != nil {
			return false, err
		}
		files = append(files, cmdlineFile)
	}

	// This is needed because imgs from GCS are compressed and we cannot get the correct size of the uncompressed images, so we have to download them first.
	// TODO(ihuh): We should enable this step as a command line option.
	workdir, err := os.MkdirTemp("", "working-dir")
	if err != nil {
		return false, err
	}
	defer os.RemoveAll(workdir)
	imgs, closeFunc, err := downloadImagesToDir(ctx, workdir, imgs)
	if err != nil {
		return false, err
	}
	defer closeFunc()

	for _, img := range imgs {
		for _, arg := range img.Args {
			name, ok := bootserverArgToName(arg)
			if !ok {
				return false, fmt.Errorf("unrecognized bootserver argument found: %s", arg)
			}
			imgFile, err := newNetsvcFile(name, img.Reader, img.Size)
			if err != nil {
				return false, err
			}
			files = append(files, imgFile)
		}
	}

	// Convert the authorized keys into a netsvc file.
	if len(authorizedKeys) > 0 {
		reader := bytes.NewReader(authorizedKeys)
		authorizedKeysFile, err := newNetsvcFile(constants.AuthorizedKeysNetsvcName, reader, reader.Size())
		if err != nil {
			return false, err
		}
		files = append(files, authorizedKeysFile)
	}

	sort.Slice(files, func(i, j int) bool {
		// Firmware files share an index. Functionally the order doesn't matter
		// but it makes test verification a lot simpler if we apply a fixed
		// ordering, so sort equal index files by name.
		if files[i].index == files[j].index {
			return files[i].name < files[j].name
		}
		return files[i].index < files[j].index
	})

	if len(files) == 0 {
		return false, errors.New("no files to transfer")
	}
	if err := transfer(ctx, t, files); err != nil {
		return false, err
	}

	// If we do not load a kernel into RAM, then we reboot back into the first kernel
	// partition; else we boot directly from RAM.
	// TODO(fxbug.dev/31931): Eventually, no such kernel should be present.
	hasRAMKernel := files[len(files)-1].name == constants.KernelNetsvcName
	return hasRAMKernel, err
}

// ValidateBoard reads the board info from the target and validates that it matches boardName.
func ValidateBoard(ctx context.Context, t tftp.Client, boardName string) error {
	// Attempt to read a file. If the server tells us we need to wait, then try
	// again as long as it keeps telling us this. ErrShouldWait implies the server
	// is still responding and will eventually be able to handle our request.
	logger.Debugf(ctx, "attempting to read %s...", constants.BoardInfoNetsvcName)
	var r *bytes.Reader
	if err := retry.Retry(ctx, retry.NewConstantBackoff(time.Second), func() error {
		var err error
		r, err = t.Read(ctx, constants.BoardInfoNetsvcName)
		if err != nil {
			if !errors.Is(err, tftp.ErrShouldWait) {
				return retry.Fatal(err)
			}
			// The target is busy, so let's sleep for a bit before trying again,
			// otherwise we'll be wasting cycles and printing too often.
			logger.Debugf(ctx, "target is busy, retrying in one second")
			return err
		}
		return nil
	}, nil); err != nil {
		return fmt.Errorf("Unable to read the board info from [%s]: %w", constants.BoardInfoNetsvcName, err)
	}

	buf := make([]byte, r.Size())
	if _, err := r.Read(buf); err != nil {
		return fmt.Errorf("Unable to read the board info from [%s]: %w", constants.BoardInfoNetsvcName, err)
	}
	// Get the bytes before the first null byte.
	if index := bytes.IndexAny(buf, "\x00"); index >= 0 {
		buf = buf[:index]
	}
	targetBoardName := string(buf)
	if targetBoardName != boardName {
		return fmt.Errorf("Expected target to be [%s], but found target is [%s]", boardName, targetBoardName)
	}
	return nil
}

// Boot prepares and boots a device at the given IP address.
func Boot(ctx context.Context, t tftp.Client, imgs []Image, cmdlineArgs []string, authorizedKeys []byte) error {
	logger.Debugf(ctx, "Transferring images to %s", t.RemoteAddr())
	hasRAMKernel, err := transferImages(ctx, t, imgs, cmdlineArgs, authorizedKeys)
	if err != nil {
		return err
	}
	logger.Debugf(ctx, "Done transferring images to %s", t.RemoteAddr())

	n := netboot.NewClient(time.Second)
	if hasRAMKernel {
		// Try to send the boot command a few times, as there's no ack, so it's
		// not possible to tell if it's successfully booted or not.
		for i := 0; i < 5; i++ {
			if err := n.Boot(t.RemoteAddr()); err != nil {
				return err
			}
		}
		return nil
	}
	return n.Reboot(t.RemoteAddr())
}

// A file to send to netsvc.
type netsvcFile struct {
	name   string
	reader io.ReaderAt
	index  int
	size   int64
}

func newNetsvcFile(name string, reader io.ReaderAt, size int64) (*netsvcFile, error) {
	idx, ok := transferOrder(name)
	if !ok {
		return nil, fmt.Errorf("unrecognized name: %s", name)
	}
	return &netsvcFile{
		reader: reader,
		name:   name,
		index:  idx,
		size:   size,
	}, nil
}

// Transfers files over TFTP to a node at a given address.
func transfer(ctx context.Context, t tftp.Client, files []*netsvcFile) error {
	// Attempt the whole process of sending every file over and retry on failure of any file.
	// This behavior more closely aligns with that of the bootserver.
	return retry.Retry(ctx, retry.WithMaxAttempts(retry.NewConstantBackoff(time.Second), 21), func() error {
		for _, f := range files {
			// Attempt to send a file. If the server tells us we need to wait, then try
			// again as long as it keeps telling us this. ErrShouldWait implies the server
			// is still responding and will eventually be able to handle our request.
			logger.Debugf(ctx, "attempting to send %s (%d)...", f.name, f.size)
			for {
				if ctx.Err() != nil {
					return nil
				}
				err := t.Write(ctx, f.name, f.reader, f.size)
				switch err {
				case nil:
				case tftp.ErrShouldWait:
					// The target is busy, so let's sleep for a bit before
					// trying again, otherwise we'll be wasting cycles and
					// printing too often.
					logger.Debugf(ctx, "target is busy, retrying in one second")
					time.Sleep(time.Second)
					continue
				default:
					if skipOnTransferError(f.name) {
						logger.Debugf(ctx, "%s; skipping and continuing: %v", constants.FailedToSendErrMsg(f.name), err)
					} else {
						logger.Debugf(ctx, "%s; starting from the top: %v", constants.FailedToSendErrMsg(f.name), err)
						return err
					}
				}
				break
			}
			logger.Debugf(ctx, "successfully sent %s", f.name)
		}
		return nil
	}, nil)
}
