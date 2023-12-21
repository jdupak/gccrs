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

#include "rust-borrow-checker.h"
#include "rust-function-collector.h"
#include "rust-bir-fact-collector.h"
#include "rust-bir-builder.h"
#include "rust-bir-dump.h"
#include "polonius/rust-polonius.h"

namespace Rust {
namespace HIR {

void
mkdir_wrapped (const std::string &dirname)
{
  int ret;
#ifdef _WIN32
  ret = _mkdir (dirname.c_str ());
#elif unix
  ret = mkdir (dirname.c_str (), 0775);
#elif __APPLE__
  ret = mkdir (dirname.c_str (), 0775);
#endif
  (void) ret;
}

void
dump_function_bir (const std::string &filename, BIR::Function &func,
		   const std::string &name)
{
  std::ofstream file;
  file.open (filename);
  if (file.fail ())
    {
      rust_error_at (UNKNOWN_LOCATION, "Failed to open file %s",
		     filename.c_str ());
      return;
    }
  BIR::Dump (file, func, name).go ();
  file.close ();
}

void
BorrowChecker::go (HIR::Crate &crate)
{
  std::string crate_name;

  if (enable_dump_bir)
    {
      mkdir_wrapped ("bir_dump");
      auto mappings = Analysis::Mappings::get ();
      bool ok
	= mappings->get_crate_name (crate.get_mappings ().get_crate_num (),
				    crate_name);
      rust_assert (ok);

      mkdir_wrapped ("nll_facts_gccrs");
    }

  FunctionCollector collector;
  collector.go (crate);

  for (auto func : collector.get_functions ())
    {
      printf ("\nChecking function %s\n",
	      func->get_function_name ().as_string ().c_str ());

      BIR::BuilderContext ctx;
      BIR::Builder builder (ctx);
      auto bir = builder.build (*func);

      if (enable_dump_bir)
	{
	  std::string filename = "bir_dump/" + crate_name + "."
				 + func->get_function_name ().as_string ()
				 + ".bir.dump";
	  dump_function_bir (filename, bir,
			     func->get_function_name ().as_string ());
	}

      auto facts = BIR::FactCollector::collect (bir);

      if (enable_dump_bir)
	{
	  mkdir_wrapped ("nll_facts_gccrs/"
			 + func->get_function_name ().as_string ());
	  auto make_fact_file
	    = [&] (const std::string &suffix) -> std::ofstream {
	    std::string filename = "nll_facts_gccrs/"
				   + func->get_function_name ().as_string ()
				   + "/" + suffix + ".facts";
	    std::ofstream file;
	    file.open (filename);
	    if (file.fail ())
	      {
		abort ();
	      }
	    return file;
	  };

	  auto loan_issued_at_file = make_fact_file ("loan_issued_at");
	  rust_assert (loan_issued_at_file.is_open ());
	  facts.dump_loan_issued_at (loan_issued_at_file);
	  auto loan_killed_at_file = make_fact_file ("loan_killed_at");
	  facts.dump_loan_killed_at (loan_killed_at_file);
	  auto loan_invalidated_at_file
	    = make_fact_file ("loan_invalidated_at");
	  facts.dump_loan_invalidated_at (loan_invalidated_at_file);
	  auto subset_base_file = make_fact_file ("subset_base");
	  facts.dump_subset_base (subset_base_file);
	  auto universal_region_file = make_fact_file ("universal_region");
	  facts.dump_universal_region (universal_region_file);
	  auto cfg_edge_file = make_fact_file ("cfg_edge");
	  facts.dump_cfg_edge (cfg_edge_file);
	  auto var_used_at_file = make_fact_file ("var_used_at");
	  facts.dump_var_used_at (var_used_at_file);
	  auto var_defined_at_file = make_fact_file ("var_defined_at");
	  facts.dump_var_defined_at (var_defined_at_file);
	  auto var_dropped_at_file = make_fact_file ("var_dropped_at");
	  facts.dump_var_dropped_at (var_dropped_at_file);
	  auto use_of_var_derefs_origin_file
	    = make_fact_file ("use_of_var_derefs_origin");
	  facts.dump_use_of_var_derefs_origin (use_of_var_derefs_origin_file);
	  auto drop_of_var_derefs_origin_file
	    = make_fact_file ("drop_of_var_derefs_origin");
	  facts.dump_drop_of_var_derefs_origin (drop_of_var_derefs_origin_file);
	  auto child_path_file = make_fact_file ("child_path");
	  facts.dump_child_path (child_path_file);
	  auto path_is_var_file = make_fact_file ("path_is_var");
	  facts.dump_path_is_var (path_is_var_file);
	  auto known_placeholder_subset_file
	    = make_fact_file ("known_placeholder_subset");
	  facts.dump_known_placeholder_subset (known_placeholder_subset_file);
	  auto path_moved_at_base_file = make_fact_file ("path_moved_at_base");
	  facts.dump_path_moved_at_base (path_moved_at_base_file);
	}

      auto result
	= Polonius::polonius_run (facts.freeze (), rust_be_debug_p ());

      if (result.loan_errors)
	{
	  rust_error_at (func->get_locus (), "Found loan errors in function %s",
			 func->get_function_name ().as_string ().c_str ());
	}
      if (result.subset_errors)
	{
	  rust_error_at (func->get_locus (),
			 "Found subset errors in function %s. Some lifetime "
			 "constraints need to be added.",
			 func->get_function_name ().as_string ().c_str ());
	}
      if (result.move_errors)
	{
	  rust_error_at (func->get_locus (), "Found move errors in function %s",
			 func->get_function_name ().as_string ().c_str ());
	}
    }

  for (auto closure ATTRIBUTE_UNUSED : collector.get_closures ())
    {
      rust_sorry_at (closure->get_locus (),
		     "Closure borrow checking is not implemented yet.");
    }
}

} // namespace HIR
} // namespace Rust