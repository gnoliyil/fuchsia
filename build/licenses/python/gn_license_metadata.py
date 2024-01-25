#!/usr/bin/env fuchsia-vendored-python
# Copyright 2023 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Holders for license-related GN metadata dictionaries."""

import json
import dataclasses
import logging
from pathlib import Path
from typing import Dict, List, Tuple
from gn_label import GnLabel


@dataclasses.dataclass(frozen=True)
class GnLicenseMetadata:
    """Metadata produced by //build/licenses/license.gni template"""

    target_label: GnLabel
    public_package_name: str
    license_files: Tuple[GnLabel]

    def is_license_metadata_dict(dict: Dict) -> bool:
        return "license_files" in dict

    def from_json_dict(
        dict: Dict, absolute_fuchsia_source_path: Path = None
    ) -> "GnLicenseMetadata":
        target_label = GnLabel.from_str(dict["target_label"])
        logging.debug("Loading GnLicenseMetadata for %s", target_label)
        assert (
            target_label.toolchain
        ), f"Label must have a toolchain part: {target_label}"
        assert (
            target_label.is_local_name
        ), f"Label must have a `:name` part: {target_label}"

        public_package_name = dict["public_package_name"]
        license_files = [
            target_label.create_child_from_str(
                _rebase_absolute_path(s, absolute_fuchsia_source_path)
            )
            for s in dict["license_files"]
        ]

        return GnLicenseMetadata(
            target_label=target_label,
            public_package_name=public_package_name,
            license_files=tuple(license_files),
        )


@dataclasses.dataclass
class GnApplicableLicensesMetadata:
    """Metadata produced by the GN `applicable_licenses` template parameter"""

    target_label: GnLabel
    target_type: str
    license_labels: Tuple[GnLabel]
    third_party_resources: Tuple[GnLabel]

    def is_applicable_licenses_metadata_dict(dict: Dict) -> bool:
        return "license_labels" in dict

    def is_group(self):
        return self.target_type == "group"

    def from_json_dict(
        data: Dict, absolute_fuchsia_source_path: Path = None
    ) -> "GnApplicableLicensesMetadata":
        assert isinstance(data, dict)
        target_label = GnLabel.from_str(data["target_label"])
        logging.debug(
            "Loading GnApplicableLicensesMetadata for %s", target_label
        )
        target_type = data["target_type"] if "target_type" in data else None
        assert (
            target_label.toolchain
        ), f"Label must have a toolchain part: {target_label}"
        assert (
            target_label.is_local_name
        ), f"Label must have a `:name` part: {target_label}"

        license_labels = [GnLabel.from_str(s) for s in data["license_labels"]]

        third_party_resources = []
        if "third_party_resources" in data:
            third_party_resources = [
                target_label.create_child_from_str(
                    _rebase_absolute_path(s, absolute_fuchsia_source_path)
                )
                for s in data["third_party_resources"]
            ]
        else:
            third_party_resources = []

        return GnApplicableLicensesMetadata(
            target_label=target_label,
            target_type=target_type,
            license_labels=tuple(license_labels),
            third_party_resources=tuple(third_party_resources),
        )


@dataclasses.dataclass
class GnLicenseMetadataDB:
    """An in-memory DB of licensing GN metadata"""

    """GnLicenseMetadata by the license's GN label"""
    licenses_by_label: Dict[GnLabel, GnLicenseMetadata] = dataclasses.field(
        default_factory=dict
    )
    """GnApplicableLicensesMetadata by the target label they apply to"""
    applicable_licenses_by_target: Dict[
        GnLabel, GnApplicableLicensesMetadata
    ] = dataclasses.field(default_factory=dict)

    def from_file(
        file_path: Path, fuchsia_source_path: Path
    ) -> "GnLicenseMetadataDB":
        """Loads from a json file generated by the build/licenses/license_collection.gni template"""
        logging.debug("Loading metadata from %s", file_path)

        # To resolve absolute paths, turn the source path into an absolute one
        absolute_fuchsia_source_path = fuchsia_source_path.resolve()

        with open(file_path, "r") as f:
            output = GnLicenseMetadataDB.from_json_list(
                json.load(f), fuchsia_source_path=absolute_fuchsia_source_path
            )
            logging.debug(
                f"Loaded {len(output.licenses_by_label)} licenses and {len(output.applicable_licenses_by_target)} from {file_path}"
            )
            return output

    def from_json_list(
        json_list: List, fuchsia_source_path: Path = None
    ) -> "GnLicenseMetadataDB":
        """Loads from a json list generated by the build/licenses/license_collection.gni template"""

        db = GnLicenseMetadataDB()

        assert type(json_list) is list
        for d in json_list:
            if GnLicenseMetadata.is_license_metadata_dict(d):
                db.add_license_metadata(
                    GnLicenseMetadata.from_json_dict(d, fuchsia_source_path)
                )
            elif GnApplicableLicensesMetadata.is_applicable_licenses_metadata_dict(
                d
            ):
                db.add_applicable_licenses_metadata(
                    GnApplicableLicensesMetadata.from_json_dict(
                        d, fuchsia_source_path
                    )
                )
            else:
                raise RuntimeError(f"Unexpected json element: {d}")

        # Remove applicable_licenses for targets that are license targets.
        # Those are meaningless.
        for label in db.licenses_by_label.keys():
            if label in db.applicable_licenses_by_target:
                db.applicable_licenses_by_target.pop(label)

        return db

    def add_license_metadata(self, license_metadata: GnLicenseMetadata):
        assert license_metadata.target_label not in self.licenses_by_label
        self.licenses_by_label[license_metadata.target_label] = license_metadata

    def add_applicable_licenses_metadata(
        self, application: GnApplicableLicensesMetadata
    ):
        assert (
            application.target_label not in self.applicable_licenses_by_target
        ), f"Multiple applicable_licenses metadata entries for {application.target_label} ({application.target_type}), probably due to https://fxbug.dev/42083609)."
        self.applicable_licenses_by_target[
            application.target_label
        ] = application


def _rebase_absolute_path(
    path_str: str, absolute_fuchsia_source_path: Path
) -> str:
    if absolute_fuchsia_source_path == None:
        return path_str
    path = Path(path_str)
    if path.is_relative_to(absolute_fuchsia_source_path):
        return "//" + str(path.relative_to(absolute_fuchsia_source_path))
    return path_str
