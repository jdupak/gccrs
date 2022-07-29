// Copyright (C) 2020-2022 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#include "rust-polonius.h"
#include "rust-name-resolver.h"

// Raw declarations from the rust compatibility layer
extern "C" void *
polonius_init (void);
extern "C" void
polonius_deinit (void *handle);
extern "C" void
polonius_define_var (void *handle, size_t var_id, size_t expr_id);
extern "C" void
polonius_var_used_at (void *handle, size_t var_id, size_t point_id);
extern "C" void
polonius_compute (void *handle);

/**
 * XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX
 * XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX
 * XXX  _____________________________________  XXX
 * XXX |                                     | XXX
 * XXX |                                     | XXX
 * XXX |          /!\ IMPORTANT /!\          | XXX
 * XXX |                                     | XXX
 * XXX |_____________________________________| XXX
 * XXX                                         XXX
 * XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX
 * XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX XXX
 *
 * Some functionality in polonius requires multiple calls to polonius functions.
 * The goal is for this abstraction to take care of doing the multiple calls.
 *
 * For example, if you wish to create a reference (i.e `let b = &a`) you
 * actually need to add two facts (IIUC):
 *
 *     1. loan_issued_at(origin, loan, point)
 *     2. var_used_at(origin, point)
 *
 * Remember to also check within rustc's codebase for operations related to the
 * `all_facts` field in the borrow checking context.
 *
 * The goal of this abstraction is for our BorrowChecker class to NOT perform
 * these two calls. Instead, this abstraction should expose a method like
 * `create_reference(origin, loan, point)` which then takes care of calling
 * the two appropriate FFI polonius functions we'll have created for
 * `loan_issued_at` and `var_used_at`.
 *
 * This way, this abstraction makes sense and isn't just a wrapper on the extern
 * C functions.
 */

namespace Rust {

Polonius::Polonius () { raw_handle = polonius_init (); }

Polonius::~Polonius () { polonius_deinit (raw_handle); }

void
Polonius::define_var (HirId var_id, HirId point_id)
{
  // FIXME: Is that correct?
  polonius_define_var (raw_handle, var_id, point_id);
}

void
Polonius::var_used_at (HirId var_id, HirId use_point)
{
  polonius_var_used_at (raw_handle, var_id, use_point);
}

void
Polonius::compute ()
{
  polonius_compute (raw_handle);
}

} // namespace Rust
