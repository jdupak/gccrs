// { dg-additional-options "-frust-compile-until=compilation -frust-borrowcheck" }

pub fn position_dependent_outlives(x: &mut i32, cond: bool) -> &mut i32 {
    // { dg-error "Found loan errors in function position_dependent_outlives" "" { target *-*-* } .-1 }
    let y = &mut *x;
    if cond {
        return y;
    } else {
        *x = 0;
        return x;
    }
}
