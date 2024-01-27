use crate::raw::abi::fuse_in_header;

#[derive(Debug, Default, Copy, Clone, Eq, PartialEq)]
/// Request data
pub struct Request {
    /// the unique identifier of this request.
    pub unique: u64,
    /// the uid of this request.
    pub uid: u32,
    /// the gid of this request.
    pub gid: u32,
    /// the pid of this request.
    pub pid: u32,
}

impl From<&fuse_in_header> for Request {
    fn from(header: &fuse_in_header) -> Self {
        Self {
            unique: header.unique,
            uid: header.uid,
            gid: header.gid,
            pid: header.pid,
        }
    }
}
