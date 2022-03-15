//! There are two main components to the polonius-engine library: Facts and
//! Output.

use log::{debug, info};
use polonius_engine::{AllFacts, Atom, FactTypes, Output};

use std::convert::From;

#[derive(Debug, Clone, Copy, Default)]
struct GccrsPolonius;

/// This represents an `HirId` as emitted by `gccrs`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct GccrsAtom(usize);

impl Atom for GccrsAtom {
    fn index(self) -> usize {
        self.0
    }
}

impl From<usize> for GccrsAtom {
    fn from(inner: usize) -> GccrsAtom {
        GccrsAtom(inner)
    }
}

impl From<GccrsAtom> for usize {
    fn from(atom: GccrsAtom) -> Self {
        atom.index()
    }
}

impl FactTypes for GccrsPolonius {
    // FIXME: Do we want anything else than the `HirId` here?
    type Origin = GccrsAtom;
    type Loan = GccrsAtom;
    type Point = GccrsAtom;
    type Variable = GccrsAtom;
    type Path = GccrsAtom;
}

#[repr(C)]
pub struct FfiGccrsPolonius(pub(crate) AllFacts<GccrsPolonius>);

/// # Safety
/// You *need* to call this function before any other polonius related
/// functions. It will initialize the inner content of the `handle` pointer
/// and return.
#[no_mangle]
pub unsafe extern "C" fn polonius_init() -> *mut FfiGccrsPolonius {
    env_logger::init();

    info!("initializing handle");

    let facts = AllFacts::default();

    Box::into_raw(Box::new(FfiGccrsPolonius(facts)))
}

/// # Safety
/// Call this function only on handled returned by a call to [`polonius_init`]. This
/// deinitializes the handle and needs to be called once all polonius operations have been
/// performed.
#[no_mangle]
pub unsafe extern "C" fn polonius_deinit(handle: *mut FfiGccrsPolonius) {
    info!("deinitializing handle");

    drop(Box::from_raw(handle));
}

/// # Safety
/// Do not call this function without having initialized the handle first using
/// [`polonius_init`]
#[no_mangle]
pub unsafe extern "C" fn polonius_var_used_at(
    handle: *mut FfiGccrsPolonius,
    var_id: usize,
    point_id: usize,
) {
    let handle = handle.as_mut().unwrap();

    debug!("variable {} used at {}", var_id, point_id);

    handle.0.var_used_at.push((var_id.into(), point_id.into()))
}

/// # Safety
/// Do not call this function without having initialized the handle first using
/// [`polonius_init`]
#[no_mangle]
pub unsafe extern "C" fn polonius_define_var(
    handle: *mut FfiGccrsPolonius,
    var_id: usize,
    point_id: usize,
) {
    let handle = handle.as_mut().unwrap();

    debug!("defining variable ([point] {}: [var] {})", point_id, var_id);

    handle
        .0
        .var_defined_at
        .push((var_id.into(), point_id.into()))
}

/// # Safety
/// Do not call this function without having initialized the handle first using
/// [`polonius_init`]
#[no_mangle]
pub unsafe extern "C" fn polonius_borrow_var(handle: *mut FfiGccrsPolonius) {
    let _handle = handle.as_mut().unwrap();

    debug!("borrowing variable");
}

/// # Safety
/// Do not call this function without having initialized the handle first using
/// [`polonius_init`]
#[no_mangle]
pub unsafe extern "C" fn polonius_compute(handle: *mut FfiGccrsPolonius) {
    let handle = handle.as_mut().unwrap();

    info!("computing facts");

    Output::compute(&handle.0, polonius_engine::Algorithm::Naive, true);
}
