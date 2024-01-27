# Copyright 2022 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Classes for using an SPDX document and its sub-elements"""

from collections import defaultdict
import dataclasses
import json
import re
from fuchsia.tools.licenses.common_types import *
from typing import Any, Dict, List, Set, Tuple, Type

# Actually 2.2.2, but only SPDX-N.M is used in JSON serialization.
_supported_spdx_json_version = "SPDX-2.2"
_spdx_document_ref = "SPDXRef-DOCUMENT"


@dataclasses.dataclass(frozen=True)
class SpdxLicenseExpression:
    """
    Holds an SPDX license expression string.

    Implementing https://spdx.github.io/spdx-spec/v2-draft/SPDX-license-expressions/
    is rather complex, but for our purposes we only need to extract and replace
    the ids of licenses in the expression, not parse the expression itself.
    """

    # A formatted string template. Will contain {0}, {1}, ... as placeholder for the various licenses.
    expression_template: str
    license_ids: Tuple[str]

    def create(expression_str: str, location_for_error=None):
        assert expression_str != None

        expression_template = []
        license_refs = {}

        remaining_str = expression_str
        while remaining_str:
            # Try to match LicenseRef-... or License-...:
            # Note that License- is not part of the SPDX spec, but nevertheless
            # some common SPDX libs use it.
            match = re.match(
                r"^(LicenseRef|License)-[a-zA-Z0-9-\.]+", remaining_str)
            if match:
                assert match.pos == 0
                remaining_str = remaining_str[match.end():]
                ref = match.group()
                if ref not in license_refs:
                    license_refs[ref] = len(license_refs.keys())
                expression_template.append("{%s}" % license_refs[ref])
                continue

            # Try to match other expression tokens: AND, OR, WITH, (, ), + and whitespace...
            match = re.match(r"^AND|^OR|^WITH|^\(|^\)|^\+|^\s+", remaining_str)
            if match:
                remaining_str = remaining_str[match.end():]
                assert match.pos == 0
                expression_template.append(match.group())
                continue

            raise LicenseException(
                f"Invalid license expression token '{remaining_str}'",
                location_for_error)

        # Temporary workaround for fxb/117652#c3. Only the last ref is meaningful
        if len(license_refs) > 1:
            key_list = list(license_refs.keys())
            if key_list[0].endswith("NOTICE.txt-0") and key_list[-1].endswith(
                    "LICENSE-0"):
                return SpdxLicenseExpression.create(
                    f"{key_list[0].replace('NOTICE.txt-0', 'NOTICE.txt')} AND {key_list[-1].replace('LICENSE-0', 'LICENSE')}"
                )

        return SpdxLicenseExpression(
            expression_template="".join(expression_template),
            license_ids=tuple(license_refs.keys()))

    def serialize(self):
        return self.expression_template.format(*self.license_ids)

    def replace_license_ids(self, id_replacer: "SpdxIdReplacer"):
        return dataclasses.replace(
            self,
            license_ids=tuple(
                [id_replacer.replaced_id(id) for id in self.license_ids]))


@dataclasses.dataclass(frozen=True)
class SpdxPackage:
    """Container for an SPDX package element"""

    spdx_id: str
    name: str
    copyright_text: str
    license_concluded: SpdxLicenseExpression

    def to_json_dict(self):
        output = {"SPDXID": self.spdx_id, "name": self.name}
        _maybe_set(output, "copyrightText", self.copyright_text)
        if self.license_concluded:
            output["licenseConcluded"] = self.license_concluded.serialize()
        return output

    def from_json_dict(input: DictReader):
        license_concluded_str = input.get_or("licenseConcluded", None)
        license_concluded = SpdxLicenseExpression.create(
            license_concluded_str,
            input.location) if license_concluded_str else None

        copyright_text = input.get_or("copyrightText", None)
        if copyright_text == "NOASSERTION":
            copyright_text = None

        return SpdxPackage(
            spdx_id=input.get("SPDXID"),
            name=input.get("name"),
            copyright_text=copyright_text,
            license_concluded=license_concluded,
        )

    def replace_license_ids(self, license_id_replacer: "SpdxIdReplacer"):
        if self.license_concluded:
            replaced_license_concluded = self.license_concluded.replace_license_ids(
                license_id_replacer)
            return dataclasses.replace(
                self, license_concluded=replaced_license_concluded)
        else:
            return self


@dataclasses.dataclass(frozen=True)
class SpdxExtractedLicensingInfo:
    """
    Container for an SPDX license element.

    Corresponds with SPDX 2.2.2 specification:
    https://spdx.github.io/spdx-spec/other-licensing-information-detected/
    """

    license_id: str
    name: str
    extracted_text: str
    cross_refs: List[str] = dataclasses.field(default_factory=list)
    see_also: List[str] = dataclasses.field(default_factory=list)

    def to_json_dict(self):
        output = {
            "name": self.name,
            "licenseId": self.license_id,
            "extractedText": self.extracted_text,
        }
        if self.cross_refs:
            output["crossRefs"] = [{
                "url": u,
            } for u in self.cross_refs]
        _maybe_set(output, "seeAlsos", self.see_also)

        return output

    def from_json_dict(input: DictReader):
        license_id = input.get("licenseId")
        # TODO(fxb/117652#c2): Remove fallback to license_id once Fuchsia produces names for all licenses.
        name = input.get_or("name", license_id)
        return SpdxExtractedLicensingInfo(
            license_id=license_id,
            name=name,
            extracted_text=input.get("extractedText"),
            cross_refs=[
                ref_dict.get("url")
                for ref_dict in input.get_readers_list("crossRefs")
            ],
            # 'seeAlso' sometimes appears as 'seeAlsos'
            see_also=input.get_or(
                "seeAlso", default=input.get_or("seeAlsos", default=[])))

    def merge_with(self, other: "SpdxExtractedLicensingInfo"):
        unified_cross_refs = _unify_and_sort_lists(
            other.cross_refs, self.cross_refs)
        unified_see_also = _unify_and_sort_lists(other.see_also, self.see_also)

        return dataclasses.replace(
            self, cross_refs=unified_cross_refs, see_also=unified_see_also)

    def extracted_text_lines(self):
        return self.extracted_text.splitlines()


@dataclasses.dataclass(frozen=True)
class SpdxRelationship:
    """Container for an SPDX relationship element"""

    spdx_element_id: str
    related_spdx_element: str
    relationship_type: str

    def to_json_dict(self):
        return {
            "spdxElementId": self.spdx_element_id,
            "relatedSpdxElement": self.related_spdx_element,
            "relationshipType": self.relationship_type
        }

    def from_json_dict(input: DictReader):
        return SpdxRelationship(
            spdx_element_id=input.get("spdxElementId"),
            related_spdx_element=input.get("relatedSpdxElement"),
            relationship_type=input.get("relationshipType"))


@dataclasses.dataclass(frozen=True)
class SpdxDocument:
    """Container for an SPDX document element"""

    file_path: str
    name: str
    namespace: str
    creators: List[str]
    describes: List[str]
    packages: List[SpdxPackage]
    relationships: List[SpdxRelationship]
    extracted_licenses: List[SpdxExtractedLicensingInfo]
    spdx_id: str = _spdx_document_ref

    def refactor_ids(self, package_id_factory, license_id_factory):
        """
        Returns a copy of the document with all ids refactored.

        Uses the input id factories to replace existing package and license
        ids in the doc with new ones, and fixes all id references.
        """

        package_id_replacer = SpdxIdReplacer(package_id_factory, self.file_path)
        license_id_replacer = SpdxIdReplacer(license_id_factory, self.file_path)

        new_extracted_licenses = [
            dataclasses.replace(
                el, license_id=license_id_replacer.new_id(el.license_id))
            for el in self.extracted_licenses
        ]
        new_packages = []
        for p in self.packages:
            p = p.replace_license_ids(license_id_replacer)
            new_packages.append(
                dataclasses.replace(
                    p, spdx_id=package_id_replacer.new_id(p.spdx_id)))

        new_describes = [
            package_id_replacer.replaced_id(d) for d in self.describes
        ]
        new_relationships = [
            dataclasses.replace(
                r,
                spdx_element_id=package_id_replacer.replaced_id(
                    r.spdx_element_id),
                related_spdx_element=package_id_replacer.replaced_id(
                    r.related_spdx_element)) for r in self.relationships
        ]
        return dataclasses.replace(
            self,
            describes=new_describes,
            packages=new_packages,
            relationships=new_relationships,
            extracted_licenses=new_extracted_licenses)

    def to_json(self, spdx_json_file_path):
        json_dict = self.to_json_dict()
        with open(spdx_json_file_path, 'w') as output_file:
            json.dump(json_dict, output_file, indent=4)

    def to_json_dict(self):
        return {
            "spdxVersion":
                _supported_spdx_json_version,
            "SPDXID":
                self.spdx_id,
            "name":
                self.name,
            "documentNamespace":
                self.namespace,
            "creationInfo": {
                "creators": self.creators,
            },
            "dataLicense":
                "CC0-1.0",
            "documentDescribes":
                self.describes,
            "packages": [p.to_json_dict() for p in self.packages],
            "relationships": [r.to_json_dict() for r in self.relationships],
            "hasExtractedLicensingInfos":
                [e.to_json_dict() for e in self.extracted_licenses],
        }

    def from_json(spdx_json_file_path: str):
        input_file = open(spdx_json_file_path, 'r')
        doc_dict = DictReader(json.load(input_file), f"{spdx_json_file_path}")
        return SpdxDocument.from_json_dict(spdx_json_file_path, doc_dict)

    def from_json_dict(spdx_json_file_path, doc_dict: DictReader):
        """Parses an SPDX json dictionary into an SpdxDocument"""

        name = doc_dict.get("name")
        document_spdx_id = doc_dict.get("SPDXID")
        namespace = doc_dict.get("documentNamespace")
        spdx_version = doc_dict.get("spdxVersion")
        if spdx_version != _supported_spdx_json_version:
            raise LicenseException(
                f"Only {_supported_spdx_json_version} is supported but {spdx_version} found",
                doc_dict.location)
        creators = doc_dict.get_reader("creationInfo").get(
            "creators", expected_type=list)

        describes = doc_dict.get_or("documentDescribes", [], expected_type=list)
        packages = [
            SpdxPackage.from_json_dict(d)
            for d in doc_dict.get_readers_list("packages", dedup=True)
        ]
        relationships = [
            SpdxRelationship.from_json_dict(d)
            for d in doc_dict.get_readers_list("relationships", dedup=True)
        ]
        # Ignore relationships between the document and packages - we don't care for these
        relationships = [
            r for r in relationships
            if r.spdx_element_id != document_spdx_id and
            r.related_spdx_element != document_spdx_id
        ]

        extracted_licenses = [
            SpdxExtractedLicensingInfo.from_json_dict(d)
            for d in doc_dict.get_readers_list(
                "hasExtractedLicensingInfos", dedup=True)
        ]

        return SpdxDocument(
            file_path=spdx_json_file_path,
            name=name,
            namespace=namespace,
            creators=creators,
            describes=describes,
            packages=packages,
            relationships=relationships,
            extracted_licenses=extracted_licenses,
            spdx_id=document_spdx_id)


class SpdxIndex:
    """Builds an index for optimized lookup across an SpdxDocument"""

    def __init__(
            self, spdx_doc_file_path: str,
            license_by_id: Dict[str, SpdxExtractedLicensingInfo],
            package_by_id: Dict[str, SpdxPackage],
            packages_by_license_id: Dict[str, Set[str]],
            child_packages_by_parent_id: Dict[str, Set[str]],
            parent_packages_by_child_id: Dict[str, Set[str]]):
        self._spdx_doc_file_path = spdx_doc_file_path
        self._license_by_id = license_by_id
        self._package_by_id = package_by_id
        self._packages_by_license_id = packages_by_license_id
        self._child_packages_by_parent_id = child_packages_by_parent_id
        self._parent_packages_by_child_id = parent_packages_by_child_id

    def get_root_packages(self):
        return [
            p for p in self._package_by_id.values()
            if not self.get_parent_packages(p)
        ]

    def get_packages_by_license(self, license):
        id = license.license_id
        if id in self._packages_by_license_id:
            return self.get_packages_by_ids(self._packages_by_license_id[id])
        else:
            raise LicenseException(
                f"No packages associated with '{license}",
                self._spdx_doc_file_path)

    def get_license_by_id(self, id):
        if id in self._license_by_id:
            return self._license_by_id[id]
        else:
            raise LicenseException(
                f"No license with id '{id}", self._spdx_doc_file_path)

    def get_package_by_id(self, id):
        if id in self._package_by_id:
            return self._package_by_id[id]
        else:
            raise LicenseException(
                f"No package with id '{id}", self._spdx_doc_file_path)

    def get_packages_by_ids(self, ids):
        return [self.get_package_by_id(id) for id in ids]

    def get_parent_packages(self, package):
        id = package.spdx_id
        if id in self._parent_packages_by_child_id:
            return self.get_packages_by_ids(
                self._parent_packages_by_child_id[id])
        else:
            return []

    def get_child_packages(self, package):
        id = package.spdx_id
        if id in self._child_packages_by_parent_id:
            return self.get_packages_by_ids(
                self._child_packages_by_parent_id[id])
        else:
            return []

    def dependency_chains_for_license(
            self, license_id: str) -> List[List[SpdxPackage]]:
        """"
        Computes all the dependencies of a given license.

        Returns a list of list of packages. Each list of packages is a dependency chain
        from the root of the SPDX document to the license.
        """

        def path_recursion(
                current_path: List[SpdxPackage], current_package: SpdxPackage):
            parents = self.get_parent_packages(current_package)
            if not parents:
                # End of the chain: Output the current path in reverse
                path = current_path[::-1]
                output.append(path)
            else:
                for p in parents:
                    current_path.append(p)
                    path_recursion(current_path, p)
                    current_path.pop()

        output = []

        license = self.get_license_by_id(license_id)
        for p in self.get_packages_by_license(license):
            path_recursion(current_path=[p], current_package=p)

        return output

    def create(input: SpdxDocument):
        """Constructs an SpdxIndex for the given SpdxDocument"""
        license_by_id = {}
        for el in input.extracted_licenses:
            if el.license_id in license_by_id:
                raise LicenseException(
                    f"license id '{el.license_id}' defined multiple times",
                    input.file_path)
            license_by_id[el.license_id] = el

        package_by_id = {}
        packages_by_license_id = defaultdict(set)
        for p in input.packages:
            id = p.spdx_id
            if id in package_by_id:
                raise LicenseException(
                    f"spdx id {id} defined multiple times", input.file_path)
            package_by_id[id] = p

            if p.license_concluded:
                for license_id in p.license_concluded.license_ids:
                    if license_id not in license_by_id:
                        raise LicenseException(
                            f"license_conclude '{license_id}' used but no such license defined",
                            input.file_path)
                    packages_by_license_id[license_id].add(id)

        child_packages_by_parent_id = defaultdict(set)
        parent_packages_by_child_id = defaultdict(set)

        for r in input.relationships:
            parent = r.spdx_element_id
            child = r.related_spdx_element
            if parent == input.spdx_id or child == input.spdx_id:
                # Ignore relationship to the document itself
                continue
            if parent not in package_by_id:
                raise LicenseException(
                    f"spdx id '{parent}' used in relationship but there is no element with that id",
                    input.file_path)
            if child not in package_by_id:
                raise LicenseException(
                    f"spdx id '{child}' used in relationship but there is no element with that id",
                    input.file_path)
            if r.relationship_type == 'CONTAINS':
                child_packages_by_parent_id[parent].add(child)
                parent_packages_by_child_id[child].add(parent)

        return SpdxIndex(
            spdx_doc_file_path=input.file_path,
            license_by_id=license_by_id,
            package_by_id=package_by_id,
            packages_by_license_id=packages_by_license_id,
            child_packages_by_parent_id=child_packages_by_parent_id,
            parent_packages_by_child_id=parent_packages_by_child_id,
        )


class SpdxIdFactory:
    """Factory for SPDX ids"""

    _next_id: int

    def __init__(self, id_template):
        self.id_template = id_template
        self._next_id = -1

    def new_id(self):
        self._next_id = self._next_id + 1
        return self.id_template.format(id=self._next_id)

    def new_package_id_factory():
        """A factory for SPDXIDs for packages"""
        return SpdxIdFactory("SPDXRef-Package-{id}")

    def new_license_id_factory():
        """A factory for LicenseRef ids"""
        return SpdxIdFactory("LicenseRef-{id}")


class SpdxIdReplacer:
    """Helper for replacing Spdx Ids"""

    _replaced_ids: Dict[str, str]

    def __init__(self, id_factory: SpdxIdFactory, doc_location: str = None):
        self._id_factory = id_factory
        self._doc_location = doc_location
        self._replaced_ids = {}

    def new_id(self, old_id):
        """Returns a new id"""
        if old_id in self._replaced_ids:
            raise LicenseException(
                f"'{old_id}' declared twice", self._doc_location)
        new_id = self._id_factory.new_id()
        self._replaced_ids[old_id] = new_id
        return new_id

    def map_id(self, old_id, new_id):
        """Maps an old id to a predefined new id"""
        if old_id in self._replaced_ids:
            raise LicenseException(
                f"'{old_id}' already mapped", self._doc_location)
        self._replaced_ids[old_id] = new_id

    def replaced_id(self, old_id):
        """Returns the new id associated with the given id"""
        if old_id is None:
            return old_id
        if old_id not in self._replaced_ids:
            raise LicenseException(
                f"Spdx id '{old_id}' doesn't refer to any known element",
                self._doc_location)
        return self._replaced_ids[old_id]


def _maybe_set(output_dict: Dict[str, Any], key: str, value: Any):
    if value:
        output_dict[key] = value


def _unify_and_sort_lists(list1, list2):
    """Unifies and sorts 2 lists, removing duplicate values"""
    unique_values = set()
    if list1:
        unique_values.update(list1)
    if list2:
        unique_values.update(list2)
    return sorted(list(unique_values))
