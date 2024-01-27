# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import collections
import functools
import json


class File(object):
    '''Wrapper class for file definitions.'''

    def __init__(self, json):
        self.source = json['source']
        self.destination = json['destination']

    def __str__(self):
        return '{%s <-- %s}' % (self.destination, self.source)


@functools.total_ordering
class Atom(object):
    '''Wrapper class for atom data, adding convenience methods.'''

    def __init__(self, json):
        self.json = json
        self.id = json['id']
        self.metadata = json['meta']
        self.label = json['gn-label']
        self.category = json['category']
        self.deps = json['deps']
        self.files = [File(f) for f in json['files']]
        self.type = json['type']

    def __str__(self):
        return str(self.id)

    def __hash__(self):
        return hash(self.label)

    def __eq__(self, other):
        return self.label == other.label

    def __ne__(self, other):
        return not __eq__(self, other)

    def __lt__(self, other):
        return self.id < other.id


def gather_dependencies(manifests):
    '''Extracts the set of all required atoms from the given manifests, as well
       as the set of names of all the direct dependencies.
       '''
    direct_deps = set()
    atoms = set()

    if manifests is None:
        return (direct_deps, atoms)

    for dep in manifests:
        with open(dep, 'r') as dep_file:
            dep_manifest = json.load(dep_file)
            direct_deps.update(dep_manifest['ids'])
            atoms.update([Atom(a) for a in dep_manifest['atoms']])
    return (direct_deps, atoms)


def detect_collisions(atoms):
    '''Detects name collisions in a given atom list.'''
    mappings = collections.defaultdict(lambda: [])
    for atom in atoms:
        mappings[atom.id].append(atom)
    has_collisions = False
    for id, group in mappings.items():
        if len(group) == 1:
            continue
        has_collisions = True
        labels = [a.label for a in group]
        print('Targets sharing the SDK id %s:' % id)
        for label in labels:
            print(' - %s' % label)
    return has_collisions


CATEGORIES = [
    'excluded',
    'experimental',
    'internal',
    'cts',
    'partner_internal',
    'partner',
    'public',
]


def index_for_category(category):
    if not category in CATEGORIES:
        raise Exception('Unknown SDK category "%s"' % category)
    return CATEGORIES.index(category)


def detect_category_violations(category, atoms):
    '''Detects mismatches in publication categories.'''
    has_violations = False
    category_index = index_for_category(category)
    for atom in atoms:
        if index_for_category(atom.category) < category_index:
            has_violations = True
            print(
                '%s has publication level %s, incompatible with %s' %
                (atom, atom.category, category))
    return has_violations
