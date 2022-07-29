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

#ifndef RUST_POLONIUS_H
#define RUST_POLONIUS_H

#include "rust-hir-expr.h"
#include "rust-hir-item.h"
#include "rust-name-resolver.h"

namespace Rust {
class Polonius
{
public:
  Polonius ();
  ~Polonius ();

  /**
   * Define a new variable to the polonius engine
   *
   * @param var Variable to define
   * @param point Initialization point
   */
  void define_var (HirId var_id, HirId point_id);

  /**
   * Define a use-site for an existing variable
   */
  // FIXME: ARTHUR: Missing a parameter?
  void var_used_at (HirId var_id, HirId use_point);

  /**
   * Compute polonius results using the handle
   */
  void compute ();

private:
  void *raw_handle;
};

} // namespace Rust

#endif /* !RUST_POLONIUS_H */
