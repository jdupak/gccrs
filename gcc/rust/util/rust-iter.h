#ifndef RUST_ITER_H
#define RUST_ITER_H

namespace Rust {
template <typename Iterable, typename T>
tl::optional<T &>
find (Iterable &iterable, const T &needle)
{
  auto it = std::find (iterable.begin (), iterable.end (), needle);
  if (it == iterable.end ())
    {
      return tl::nullopt;
    }
  return *it;
}

template <typename Iterable, typename T>
bool
contains (Iterable &iterable, const T &needle)
{
  return find (iterable, needle).has_value ();
}

template <typename T, typename F>
bool
any_of (const T &container, F predicate)
{
  return std::any_of (container.begin (), container.end (), predicate);
}

} // namespace Rust

#endif // RUST_ITER_H
