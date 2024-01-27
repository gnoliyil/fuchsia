// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_merkle::Hash as FuchsiaMerkleHash;
use std::fmt;

#[derive(Clone, Debug, Eq, PartialEq, Hash)]
pub struct Hash(FuchsiaMerkleHash);

#[cfg(test)]
impl Hash {
    /// Constructs a [`Hash`] that represents the contents read from `contents.
    pub fn from_contents<R: std::io::Read>(contents: R) -> Self {
        Self(
            fuchsia_merkle::MerkleTree::from_reader(contents)
                .expect("compute fuchsia merkle tree")
                .root(),
        )
    }
}

impl From<FuchsiaMerkleHash> for Hash {
    fn from(fuchsia_merkle_hash: FuchsiaMerkleHash) -> Self {
        Self(fuchsia_merkle_hash)
    }
}

impl Into<FuchsiaMerkleHash> for Hash {
    fn into(self) -> FuchsiaMerkleHash {
        self.0
    }
}

impl fmt::Display for Hash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

#[cfg(test)]
mod tests {
    use super::Hash;
    use fuchsia_merkle::MerkleTree as FuchsiaMerkleTree;

    #[fuchsia::test]
    fn test_hex_merkle_root_fmt() {
        let contents = "hello_world";
        let hash = Hash::from_contents(contents.as_bytes());
        let merkle_root = FuchsiaMerkleTree::from_reader(contents.as_bytes()).unwrap().root();
        assert_eq!(format!("{}", hash), format!("{}", merkle_root));
    }

    #[fuchsia::test]
    fn test_equality() {
        let hello1 = Hash::from_contents("hello".as_bytes());
        let hello2 = Hash::from_contents("hello".as_bytes());
        let goodbye = Hash::from_contents("goodbye".as_bytes());
        assert_eq!(hello1, hello2);
        assert_ne!(hello1, goodbye)
    }
}

#[cfg(test)]
pub mod test {
    use super::Hash;
    use fuchsia_merkle::MerkleTree as FuchsiaMerkleTree;
    use std::ops::Add;

    pub trait HashGenerator: Default {
        fn next(self) -> Self;
    }

    impl<T: Add<Output = T> + Default + From<u8>> HashGenerator for T {
        fn next(self) -> Self {
            self + (1 as u8).into()
        }
    }

    impl Default for Hash {
        fn default() -> Self {
            Hash::from(FuchsiaMerkleTree::from_reader("".as_bytes()).unwrap().root())
        }
    }

    impl HashGenerator for Hash {
        fn next(self) -> Self {
            Hash::from(FuchsiaMerkleTree::from_reader(self.0.as_bytes()).unwrap().root())
        }
    }
}

#[cfg(test)]
pub mod fake {
    pub type Hash = u32;
}
