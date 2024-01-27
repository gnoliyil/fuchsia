// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a helper script that regenerates the initial test metadata.

package main

import (
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"time"

	tuf "github.com/theupdateframework/go-tuf"
	tufData "github.com/theupdateframework/go-tuf/data"
	tufKeys "github.com/theupdateframework/go-tuf/pkg/keys"
)

var expirationDate = time.Date(2100, time.January, 1, 0, 0, 0, 0, time.UTC)

func loadKeys(repo *tuf.Repo, role string, path string) {
	f, err := os.Open(path)
	if err != nil {
		log.Fatal(err)
	}

	var persistedKeys struct {
		Encrypted bool                  `json:"encrypted"`
		Data      []*tufData.PrivateKey `json:"data"`
	}

	if err := json.NewDecoder(f).Decode(&persistedKeys); err != nil {
		log.Fatal(err)
	}

	for _, key := range persistedKeys.Data {
		signer, err := tufKeys.GetSigner(key)
		if err != nil {
			log.Fatalf("failed to load private key: %s", err)
		}

		if err := repo.AddPrivateKeyWithExpires(role, signer, expirationDate); err != nil {
			log.Fatalf("failed to add private key: %s", err)
		}
	}
}

func main() {
	tempdir, err := os.MkdirTemp(".", "")
	if err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(tempdir)

	repo, err := tuf.NewRepo(tuf.FileSystemStore(tempdir, nil))
	if err != nil {
		log.Fatalf("failed to create repository: %s", err)
	}

	if err := repo.Init(true); err != nil {
		log.Fatalf("failed init init: %s", err)
	}

	loadKeys(repo, "root", "keys/root.json")
	loadKeys(repo, "targets", "keys/targets.json")
	loadKeys(repo, "snapshot", "keys/snapshot.json")
	loadKeys(repo, "timestamp", "keys/timestamp.json")

	if repo.AddTargetsWithExpires([]string{}, nil, expirationDate); err != nil {
		log.Fatalf("failed to create targets: %s", err)
	}

	if err := repo.SnapshotWithExpires(expirationDate); err != nil {
		log.Fatalf("failed to create snapshot: %s", err)
	}

	if err := repo.TimestampWithExpires(expirationDate); err != nil {
		log.Fatalf("failed to create timestamp: %s", err)
	}

	if err := repo.Commit(); err != nil {
		log.Fatalf("failed to commit: %s", err)
	}

	// Delete the old metadata.
	if _, err := os.Stat("repository"); err == nil {
		if err := os.RemoveAll("repository"); err != nil {
			log.Fatal(err)
		}
	}

	if err := os.Rename(filepath.Join(tempdir, "repository"), "repository"); err != nil {
		log.Fatal(err)
	}
}
