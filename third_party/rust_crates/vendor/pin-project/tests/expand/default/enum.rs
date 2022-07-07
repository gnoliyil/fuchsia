use pin_project::pin_project;

#[pin_project(project = EnumProj, project_ref = EnumProjRef)]
enum Enum<T, U> {
    Struct {
        #[pin]
        pinned: T,
        unpinned: U,
    },
    Tuple(#[pin] T, U),
    Unit,
}

fn main() {}
