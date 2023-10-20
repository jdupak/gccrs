// Copyright (C) 2020-2023 Free Software Foundation, Inc.

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

#ifndef RUST_BIR_FACT_COLLECTOR_H
#define RUST_BIR_FACT_COLLECTOR_H

#include <numeric>
#include "rust-bir-visitor.h"
#include "rust-bir.h"
#include "rust-bir-place.h"
#include "polonius/rust-polonius.h"

namespace Rust {
namespace BIR {

enum PointPosition
{
  START,
  MID
};

class FactCollector : public Visitor
{
  Polonius::Facts facts;

  const PlaceDB &place_db;
  const std::vector<BasicBlock> &basic_blocks;
  const std::vector<PlaceId> &arguments;
  size_t num_lifetimes;

  BasicBlockId current_bb = 0;
  uint32_t current_stmt = 0;
  PlaceId lhs = INVALID_PLACE;

public:
  explicit FactCollector (const Function &func)
    : place_db (func.place_db), basic_blocks (func.basic_blocks),
      arguments (func.arguments), num_lifetimes (func.num_lifetimes)
  {}

  Polonius::Facts go ()
  {
    collect_place_facts ();
    collect_origins ();

    for (current_bb = 0; current_bb < basic_blocks.size (); ++current_bb)
      {
	auto &bb = basic_blocks[current_bb];
	for (current_stmt = 0; current_stmt < bb.statements.size ();
	     ++current_stmt)
	  {
	    add_stmt_to_cfg (current_bb, current_stmt);
	    auto &stmt = bb.statements[current_stmt];
	    visit (stmt);
	  }
      }

    return std::move (facts);
  }

protected:
  void collect_place_facts ()
  {
    for (BIR::PlaceId place_id = 0; place_id < place_db.size (); ++place_id)
      {
	auto &place = place_db[place_id];
	switch (place.kind)
	  {
	  case BIR::Place::VARIABLE:
	  case BIR::Place::TEMPORARY:
	    facts.path_is_var.emplace_back (place_id, place_id);
	    break;
	  case BIR::Place::FIELD:
	  case BIR::Place::INDEX:
	  case BIR::Place::DEREF:
	    facts.child_path.emplace_back (place_id, place.path.parent);
	    break;
	  default:
	    break;
	  }
      }
  }
  void collect_origins ()
  {
    facts.universal_region.resize (num_lifetimes);
    std::iota (facts.universal_region.begin (), facts.universal_region.end (),
	       0);
  }

private:
  [[nodiscard]] const BasicBlock &get_current_bb () const
  {
    return basic_blocks[current_bb];
  }

  [[nodiscard]] static Polonius::Point
  get_point (BasicBlockId bb, uint32_t stmt, PointPosition pos)
  {
    return (Polonius::Point) bb << 32 | stmt << 1 | pos;
  }

  [[nodiscard]] Polonius::Point get_current_point_start () const
  {
    return get_point (current_bb, current_stmt, PointPosition::START);
  }

  [[nodiscard]] Polonius::Point get_current_point_mid () const
  {
    return get_point (current_bb, current_stmt, PointPosition::MID);
  }

  void add_stmt_to_cfg (BasicBlockId bb, uint32_t stmt)
  {
    if (current_stmt != 0)
      {
	facts.cfg_edge.emplace_back (get_point (bb, stmt - 1,
						PointPosition::MID),
				     get_point (bb, stmt,
						PointPosition::START));
      }

    facts.cfg_edge.emplace_back (get_current_point_start (),
				 get_current_point_mid ());
  }

  void issue_jumps ()
  {
    for (auto succ : get_current_bb ().successors)
      facts.cfg_edge.emplace_back (get_current_point_start (),
				   get_point (succ, 0, PointPosition::START));
  }

  void issue_read (PlaceId place_id)
  {
    const auto &place = place_db[place_id];

    if (place.kind == Place::CONSTANT)
      return;

    facts.path_accessed_at_base.emplace_back (place_id,
					      get_current_point_mid ());
    if (place.kind == Place::VARIABLE)
      facts.var_used_at.emplace_back (place_id, get_current_point_mid ());
    if (place.is_rvalue () || !place.is_copy)
      {
	facts.path_moved_at_base.emplace_back (place_id,
					       get_current_point_mid ());
	place_db.for_each_path_segment (place_id, [&] (PlaceId id) {
	  if (place_db[id].kind == Place::DEREF)
	    {
	      rust_error_at (UNKNOWN_LOCATION,
			     "Cannot move from behind a reference.");
	    }
	});
      }
  }

  void issue_write (PlaceId place_id)
  {
    const auto &place = place_db[place_id];
    rust_assert (place.is_lvalue () || place.is_rvalue ());

    facts.path_assigned_at_base.emplace_back (place_id,
					      get_current_point_mid ());
    if (place.kind == Place::VARIABLE)
      facts.var_defined_at.emplace_back (place_id, get_current_point_mid ());

    place_db.for_each_path_segment (place_id, [&] (PlaceId id) {
      if (place_db[id].kind == Place::DEREF)
	{
	  auto &base = place_db[place_db[id].path.parent];
	  if (!base.tyty->as<TyTy::ReferenceType> ()->is_mutable ())
	    rust_error_at (UNKNOWN_LOCATION,
			   "Mutating content behind an immutable reference.");
	}
    });
  }

public:
  void visit (const Node &node) override
  {
    switch (node.get_kind ())
      {
      case Node::ASSIGNMENT:
	lhs = node.get_place ();
	issue_write (lhs);
	node.get_expr ().accept_vis (*this);
	break;
      case Node::SWITCH:
	issue_jumps ();
      case Node::GOTO:
	issue_read (node.get_place ());
	issue_jumps ();
	break;
      case Node::RETURN:
	break;
      case Node::STORAGE_DEAD:
	facts.var_dropped_at.emplace_back (node.get_place (),
					   get_current_point_mid ());
	break;
      case Node::STORAGE_LIVE:
	facts.var_defined_at.emplace_back (node.get_place (),
					   get_current_point_mid ());
	break;
      }
  }
  void visit (const InitializerExpr &expr) override
  {
    for (auto init_value : expr.get_values ())
      issue_read (init_value);
  }
  void visit (const Operator<1> &expr) override
  {
    issue_read (expr.get_operand<0> ());
  }
  void visit (const Operator<2> &expr) override
  {
    issue_read (expr.get_operand<0> ());
    issue_read (expr.get_operand<1> ());
  }
  void visit (const BorrowExpr &expr) override
  {
    auto loan = lhs;
    auto lifetime = place_db[loan].lifetime;
    facts.loan_issued_at.emplace_back (lifetime.id, loan,
				       get_current_point_mid ());

    facts.use_of_var_derefs_origin.emplace_back (lhs, lifetime.id);
    place_db.for_each_path_from_root (lhs, [&] (PlaceId place_id) {
      facts.use_of_var_derefs_origin.emplace_back (place_id, lifetime.id);
    });
  }
  void visit (const Assignment &expr) override { issue_read (expr.get_rhs ()); }
  void visit (const CallExpr &expr) override
  {
    issue_read (expr.get_callable ());
    for (auto arg : expr.get_arguments ())
      issue_read (arg);
    issue_jumps ();
  }
};

} // namespace BIR
} // namespace Rust

#endif // RUST_BIR_FACT_COLLECTOR_H
