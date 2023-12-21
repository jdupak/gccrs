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

enum class PointPosition : uint8_t
{
  START,
  MID
};

class FactCollector : public Visitor
{
  // Output.
  Polonius::Facts facts;

  // Read-only context.
  const PlaceDB &place_db;
  const std::vector<BasicBlock> &basic_blocks;

  // Collector state.
  BasicBlockId current_bb = 0;
  uint32_t current_stmt = 0;
  PlaceId lhs = INVALID_PLACE;

  // PlaceDB is const in this phase, so this is used to generate fresh regions.
  FreeRegion next_fresh_region;

  std::vector<Polonius::Point> cfg_points_all;

  FreeRegion get_next_free_region () { return next_fresh_region++; }

  FreeRegions bind_regions (std::vector<TyTy::Region> regions,
			    FreeRegions parent_free_regions)
  {
    std::vector<FreeRegion> free_regions;
    for (auto &region : regions)
      {
	if (region.is_early_bound ())
	  {
	    free_regions.push_back (parent_free_regions[region.get_index ()]);
	  }
	else if (region.is_static ())
	  {
	    free_regions.push_back (0);
	  }
	else if (region.is_anonymous ())
	  {
	    free_regions.push_back (get_next_free_region ());
	  }
	else if (region.is_named ())
	  {
	    rust_unreachable (); // FIXME
	  }
	else
	  {
	    rust_sorry_at (UNKNOWN_LOCATION, "Unimplemented");
	    rust_unreachable ();
	  }
      }
    return free_regions;
  }

  FreeRegions make_fresh_regions (size_t size)
  {
    std::vector<FreeRegion> free_regions;
    for (size_t i = 0; i < size; i++)
      {
	free_regions.push_back (get_next_free_region ());
      }
    return FreeRegions (std::move (free_regions));
  }

public:
  static Polonius::Facts collect (const Function &func)
  {
    FactCollector collector (func);
    collector.init_universal_regions (func.universal_regions,
				      func.universal_region_bounds);

    collector.visit_statemensts ();
    collector.visit_places ();
    return std::move (collector.facts);
  }

protected: // Constructor and destructor.
  explicit FactCollector (const Function &func)
    : place_db (func.place_db), basic_blocks (func.basic_blocks),
      next_fresh_region (place_db.peek_next_free_region ())
  {}

  ~FactCollector () = default;

protected: // Main collection entry points (for different categories).
  void init_universal_regions (
    FreeRegions universal_regions,
    const decltype (Function::universal_region_bounds) &universal_region_bounds)
  {
    for (auto &region : universal_regions)
      facts.universal_region.emplace_back (region);

    // Copy already collected subset facts, that are universally valid.
    for (auto &bound : universal_region_bounds)
      facts.known_placeholder_subset.emplace_back (bound.first, bound.second);
  }

  void visit_places ()
  {
    for (PlaceId place_id = 0; place_id < place_db.size (); ++place_id)
      {
	auto &place = place_db[place_id];

	switch (place.kind)
	  {
	  case Place::VARIABLE:
	  case Place::TEMPORARY:
	    facts.path_is_var.emplace_back (place_id, place_id);
	    for (auto &region : place.regions)
	      {
		facts.use_of_var_derefs_origin.emplace_back (place_id, region);
	      }
	    // TODO: drop_of_var_derefs_origin
	    break;
	  case Place::FIELD:
	    sanizite_field (place_id);
	    facts.child_path.emplace_back (place_id, place.path.parent);
	    break;
	  case Place::INDEX:
	    push_subset_all (place.tyty, place.regions,
			     place_db[place.path.parent].regions);
	    facts.child_path.emplace_back (place_id, place.path.parent);
	    break;
	  case Place::DEREF:
	    sanitize_deref (place_id);
	    facts.child_path.emplace_back (place_id, place.path.parent);
	    break;
	  case Place::CONSTANT:
	  case Place::INVALID:
	    break;
	  }
      }
  }

  void sanitize_deref (PlaceId place_id)
  {
    auto &place = place_db[place_id];
    auto &base = place_db[place.path.parent];

    rust_debug ("\tSanitize deref of %s", base.tyty->as_string ().c_str ());

    std::vector<Polonius::Origin> regions;
    regions.insert (regions.end (), base.regions.begin () + 1,
		    base.regions.end ());
    push_subset_all (place.tyty, place.regions,
		     FreeRegions (std::move (regions)));
  }

  void sanizite_field (PlaceId place_id)
  {
    auto &place = place_db[place_id];
    auto &base = place_db[place.path.parent];

    rust_debug ("\tSanitize field .%d of %s", place.variable_or_field_index,
		base.tyty->as_string ().c_str ());

    if (base.tyty->is<TyTy::TupleType> ())
      return;
    auto r = TyTy::VarianceAnalysis::query_field_regions (
      base.tyty->as<TyTy::ADTType> (), 0, place.variable_or_field_index,
      base.regions); // FIXME
    FreeRegions f (std::move (r));
    push_subset_all (place.tyty, place.regions, f);
  }

  void visit_statemensts ()
  {
    rust_debug ("visit_statemensts");

    for (current_bb = 0; current_bb < basic_blocks.size (); ++current_bb)
      {
	auto &bb = basic_blocks[current_bb];
	for (current_stmt = 0; current_stmt < bb.statements.size ();
	     ++current_stmt)
	  {
	    cfg_points_all.push_back (get_current_point_start ());
	    cfg_points_all.push_back (get_current_point_mid ());

	    add_stmt_to_cfg (current_bb, current_stmt);

	    visit (bb.statements[current_stmt]);
	  }
      }
  }

protected: // Statement visitor helpers
  [[nodiscard]] const BasicBlock &get_current_bb () const
  {
    return basic_blocks[current_bb];
  }

  [[nodiscard]] static Polonius::Point
  get_point (BasicBlockId bb, uint32_t stmt, PointPosition pos)
  {
    return (Polonius::Point) bb << 16 | stmt << 1 | static_cast<uint8_t> (pos);
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

protected: // Generic BIR operations.
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

    if (place.is_var ())
      issue_var_used (place_id);

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
    if (place_id == INVALID_PLACE)
      return; // Write to `_`.

    const auto &place = place_db[place_id];
    rust_assert (place.is_lvalue () || place.is_rvalue ());

    facts.path_assigned_at_base.emplace_back (place_id,
					      get_current_point_mid ());

    issue_var_used (place_id);

    if (place.is_var ())
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

  void push_subset (FreeRegion lhs, FreeRegion rhs)
  {
    rust_debug ("\t\tpush_subset: '?%u: '?%u", lhs, rhs);

    facts.subset_base.emplace_back (lhs, rhs, get_current_point_mid ());
  }

  void push_subset_all (FreeRegion lhs, FreeRegion rhs)
  {
    rust_debug ("\t\tpush_subset_all: '?%u: '?%u", lhs, rhs);

    for (auto point : cfg_points_all)
      facts.subset_base.emplace_back (lhs, rhs, point);
  }

  void push_subset (Variance variance, FreeRegion lhs, FreeRegion rhs)
  {
    if (variance.is_covariant ())
      {
	push_subset (lhs, rhs);
      }
    else if (variance.is_contravariant ())
      {
	push_subset (rhs, lhs);
      }
    else if (variance.is_invariant ())
      {
	push_subset (lhs, rhs);
	push_subset (rhs, lhs);
      }
  }

  void push_subset_all (Variance variance, FreeRegion lhs, FreeRegion rhs)
  {
    if (variance.is_covariant ())
      {
	push_subset_all (lhs, rhs);
      }
    else if (variance.is_contravariant ())
      {
	push_subset_all (rhs, lhs);
      }
    else if (variance.is_invariant ())
      {
	push_subset_all (lhs, rhs);
	push_subset_all (rhs, lhs);
      }
  }

  void push_subset (TyTy::BaseType *type, FreeRegions lhs, FreeRegions rhs)
  {
    auto variances = TyTy::VarianceAnalysis::query_type_variances (type);
    rust_assert (lhs.size () == rhs.size ());
    rust_assert (lhs.size () == variances.size ());
    for (size_t i = 0; i < lhs.size (); ++i)
      {
	push_subset (variances[i], lhs[i], rhs[i]);
      }
  }

  void push_subset_all (TyTy::BaseType *type, FreeRegions lhs, FreeRegions rhs)
  {
    auto variances = TyTy::VarianceAnalysis::query_type_variances (type);
    rust_assert (lhs.size () == rhs.size ());
    rust_assert (lhs.size () == variances.size ());
    for (size_t i = 0; i < lhs.size (); ++i)
      {
	push_subset_all (variances[i], lhs[i], rhs[i]);
      }
  }

  void push_subset_user (TyTy::BaseType *type, FreeRegions free_regions,
			 std::vector<TyTy::Region> user_regions)
  {
    auto variances = TyTy::VarianceAnalysis::query_type_variances (type);
    rust_assert (free_regions.size () == user_regions.size ());
    rust_assert (free_regions.size () == variances.size ());

    for (size_t i = 0; i < free_regions.size (); ++i)
      {
	if (user_regions[i].is_named ())
	  {
	    push_subset (variances[i], free_regions[i],
			 {Polonius::Origin (user_regions[i].get_index ())});
	  }
	else if (user_regions[i].is_anonymous ())
	  {
	    // IGNORE
	  }
	else
	  {
	    rust_internal_error_at (UNKNOWN_LOCATION, "Unexpected region type");
	  }
      }
  }

  void issue_var_used (PlaceId place_id)
  {
    auto &place = place_db[place_id];
    if (place.is_var ())
      facts.var_used_at.emplace_back (place_id, get_current_point_mid ());
    else if (place.is_path ())
      facts.var_used_at.emplace_back (place_db.get_var (place_id),
				      get_current_point_mid ());
  }

protected: // Statement visitors.
  void visit (const Statement &stmt) override
  {
    switch (stmt.get_kind ())
      {
      case Statement::Kind::ASSIGNMENT:
	lhs = stmt.get_place ();
	issue_write (lhs);
	stmt.get_expr ().accept_vis (*this);
	break;
      case Statement::Kind::SWITCH:
	issue_jumps ();
	break;
      case Statement::Kind::GOTO:
	issue_read (stmt.get_place ());
	issue_jumps ();
	break;
      case Statement::Kind::RETURN:
	issue_var_used (RETURN_VALUE_PLACE);
	break;
      case Statement::Kind::STORAGE_DEAD:
	issue_write (stmt.get_place ());
	break;
      case Statement::Kind::STORAGE_LIVE:
	facts.var_defined_at.emplace_back (stmt.get_place (),
					   get_current_point_mid ());
	break;
	case Statement::Kind::USER_TYPE_ASCRIPTION: {
	  auto user_regions
	    = TyTy::VarianceAnalysis::query_type_regions (stmt.get_type ());
	  push_subset_user (place_db[stmt.get_place ()].tyty,
			    place_db[stmt.get_place ()].regions, user_regions);
	  break;
	}
      }
  }

  /**
   * Apply type and lifetime bounds
   *
   * For a place we have a list of fresh regions. We need to apply constraints
   * from type definition to it. First `n` regions belong to the lifetime
   * parameters of the type. The rest are flatten lifetime parameters of the
   * type arguments. We walk the type arguments with a offset
   */
  void sanitize_constrains_at_init (PlaceId place_id)
  {
    auto &place = place_db[place_id];

    rust_debug ("\tSanitize constraints of %s",
		place.tyty->as_string ().c_str ());

    if (auto generic = place.tyty->try_as<TyTy::SubstitutionRef> ())
      {
	auto &regions = place.regions;
	auto region_end = sanitize_constraints (*generic, 0, regions);
	rust_assert (region_end == regions.size ());
      }
    else if (auto ref = place.tyty->try_as<TyTy::ReferenceType> ())
      {
	for (auto &region : place.regions)
	  {
	    if (region != place.regions[0])
	      push_subset (region, place.regions[0]);
	  }
      }
  }

  size_t sanitize_constraints (const TyTy::BaseType *type, size_t region_start,
			       const FreeRegions &regions)
  {
    switch (type->get_kind ())
      {
      case TyTy::ADT:
	return sanitize_constraints (type->as<const TyTy::ADTType> (),
				     region_start, regions);
      case TyTy::STR:
	return region_start;
      case TyTy::REF:
	return 1
	       + sanitize_constraints (
		 type->as<const TyTy::ReferenceType> ()->get_base (),
		 region_start, regions);
      case TyTy::POINTER:
	return sanitize_constraints (
	  type->as<const TyTy::PointerType> ()->get_base (), region_start,
	  regions);
      case TyTy::ARRAY:
	return sanitize_constraints (
	  type->as<const TyTy::ArrayType> ()->get_element_type (), region_start,
	  regions);
      case TyTy::SLICE:
	return sanitize_constraints (
	  type->as<const TyTy::SliceType> ()->get_element_type (), region_start,
	  regions);
      case TyTy::FNDEF:
	case TyTy::TUPLE: {
	  for (auto &field : type->as<const TyTy::TupleType> ()->get_fields ())
	    sanitize_constraints (field.get_tyty (), region_start, regions);
	}
      case TyTy::FNPTR:
      case TyTy::PROJECTION:
	return sanitize_constraints (*type->as<const TyTy::SubstitutionRef> (),
				     region_start, regions);
      case TyTy::BINDER:
	return sanitize_constraints (
	  type->as<const TyTy::Binder> ()->get_bound_ty (), region_start,
	  regions);
      case TyTy::BOOL:
      case TyTy::CHAR:
      case TyTy::INT:
      case TyTy::UINT:
      case TyTy::FLOAT:
      case TyTy::USIZE:
      case TyTy::ISIZE:
      case TyTy::NEVER:
      case TyTy::DYNAMIC:
      case TyTy::CLOSURE:
      case TyTy::ERROR:
	return region_start;
      case TyTy::PLACEHOLDER:
      case TyTy::INFER:
      case TyTy::PARAM:
	rust_unreachable ();
      }
  }

  size_t sanitize_constraints (const TyTy::SubstitutionRef &type,
			       size_t region_start, const FreeRegions &regions)
  {
    for (auto constr : type.get_region_constraints ().region_region)
      {
	rust_assert (constr.first.is_early_bound ());
	rust_assert (constr.second.is_early_bound ());
	auto lhs = constr.first.get_index () + region_start;
	auto rhs = constr.second.get_index () + region_start;
	push_subset (regions[lhs], regions[rhs]);
      }

    size_t region_end = region_start + type.get_num_lifetime_params ();

    /*
     * For type `Foo<'a, T1, T2>`, where `T1 = &'b Vec<&'c i32>` and `T2 = &'d
     * i32 the regions are `['a, 'b, 'c, 'd]`. The ranges
     */
    std::vector<size_t> type_param_region_ranges;
    type_param_region_ranges.push_back (region_end);

    for (auto type_param : type.get_substs ())
      {
	TyTy::SubstitutionArg arg = TyTy::SubstitutionArg::error ();
	bool ok = type.get_used_arguments ().get_argument_for_symbol (
	  type_param.get_param_ty (), &arg);
	rust_assert (ok);
	region_end
	  = sanitize_constraints (arg.get_tyty (), region_end, regions);
	type_param_region_ranges.push_back (region_end);
      }

    /*
     * For constrain of form: `T: 'a` push outlives with all in range
     * `indexof(T)..(indexof(T) + 1)`
     */
    for (auto constr : type.get_region_constraints ().type_region)
      {
	auto type_param_index_opt
	  = type.get_used_arguments ().find_symbol (*constr.first);
	rust_assert (type_param_index_opt.has_value ());
	size_t type_param_index = type_param_index_opt.value ();

	for (size_t i = type_param_region_ranges[type_param_index];
	     i < type_param_region_ranges[type_param_index + 1]; ++i)
	  {
	    push_subset (regions[i],
			 regions[constr.second.get_index () + region_start]);
	  }
      }

    return region_end;
  }

  // TyTy::RegionParamList
  // get_field_region_params (const TyTy::ADTType *adt, size_t variant_index,
  // 	   size_t field_index,
  // 	   TyTy::RegionParamList parent_region_params)
  // {
  //
  // }

  void visit (const InitializerExpr &expr) override
  {
    sanitize_constrains_at_init (lhs);

    for (auto init_value : expr.get_values ())
      issue_read (init_value);
    // TODO: issue write? Probably not.
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
    rust_debug ("\t_%u = BorrowExpr(_%u)", lhs - 1, expr.get_place () - 1);

    auto &base_place = place_db[expr.get_place ()];
    auto &ref_place = place_db[lhs];

    facts.loan_issued_at.emplace_back (expr.get_origin (), expr.get_loan (),
				       get_current_point_mid ());

    auto loan_region = ref_place.regions[0];
    for (auto &region : base_place.regions)
      {
	push_subset (region, loan_region);
      }
  }

  void visit (const Assignment &expr) override
  {
    rust_debug ("\t_%u = Assignment(_%u) at %u:%u", lhs - 1,
		expr.get_rhs () - 1, current_bb, current_stmt);

    auto &lhs_place = place_db[lhs];
    auto &rhs_place = place_db[expr.get_rhs ()];

    issue_read (expr.get_rhs ());
    push_subset (lhs_place.tyty, lhs_place.regions, rhs_place.regions);
    issue_write (lhs);
  }

  void visit (const CallExpr &expr) override
  {
    rust_debug ("\t_%u = CallExpr(_%u)", lhs - 1, expr.get_callable () - 1);

    auto &return_place = place_db[lhs];
    auto &callable_place = place_db[expr.get_callable ()];
    auto callable_ty = callable_place.tyty->as<TyTy::CallableTypeInterface> ();

    issue_read (expr.get_callable ());

    // Each call needs unique regions.
    auto call_regions = make_fresh_regions (callable_place.regions.size ());

    for (size_t i = 0; i < expr.get_arguments ().size (); ++i)
      {
	auto arg = expr.get_arguments ().at (i);
	issue_read (arg);
	push_subset (place_db[arg].tyty,
		     bind_regions (TyTy::VarianceAnalysis::query_type_regions (
				     callable_ty->get_param_type_at (i)),
				   call_regions),
		     place_db[arg].regions);
      }

    auto return_regions
      = bind_regions (TyTy::VarianceAnalysis::query_type_regions (
			callable_ty->as<TyTy::FnType> ()->get_return_type ()),
		      call_regions);
    push_subset (return_place.tyty, return_place.regions, return_regions);
    issue_jumps ();
  }
}; // namespace BIR
} // namespace BIR
} // namespace Rust

#endif // RUST_BIR_FACT_COLLECTOR_H
