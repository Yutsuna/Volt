#pragma once

// Mangler.hpp — the one place a Volt declaration becomes a linker symbol.
//
// Shared by all three targets on purpose: the VM's FunctionTable, the wasm
// export table and LLVM's `llvm::Function` names key on the *same* string, so
// a symbol emitted by one target is recognisable to a reader of any other.
// Nothing here knows a Volt type name — a nominal contributes its declared
// spelling, whatever the stdlib called it (rules/zero-hardcode.md).
//
// The scheme is length-prefixed, so it is unambiguous without separators and
// a demangler needs no lookahead:
//
//     _V <len>Owner <len>Name [ I <arg> ... E ]
//
//   _V4Math4sqrt                   Math.sqrt (or a free function; see below)
//   _V05hello                      a free function has no owner, written `0`
//   _V5Array4pushI5Int32E          Array<Int32>#push
//   _V5Array4pushI5ArrayI5Int32EE  nesting stays explicit through I...E
//
// An `@[External]` member never reaches that scheme: its symbol is the C
// spelling recorded on the Member, verbatim, because the whole point of that
// boundary is that the linker and a C compiler agree on the name.

#include "BackendCore_export.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace Volt
{

namespace Backend
{

    // The symbol `Entry` is defined at, instantiated for `FlatArgs`.
    //
    // `Owner` is the nominal declaring the member, or an invalid id for a free
    // function. `FlatArgs` are the concrete type arguments the instantiation
    // was requested with, in **MonoRequest::Args' encoding** — a pre-order
    // walk emitting, per node, its NominalId value followed by its own
    // argument count. Sharing that one encoding is what keeps a symbol and the
    // queue key that produced it from ever disagreeing; it is also what makes
    // the nesting unambiguous, since the arity is written down rather than
    // inferred.
    //
    // Empty for a non-generic — the common case — and then no `I...E` group is
    // produced at all.
    [[nodiscard]] BACKENDCORE_EXPORT std::string MangleFunction ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                                  const MiddleEnd::TypeSystem::Member &Entry,
                                                                  MiddleEnd::TypeSystem::NominalId Owner,
                                                                  std::span<const std::uint32_t> FlatArgs );

    // The length-prefixed spelling of one nominal, without arguments. Exposed
    // because the ancestry and type tables a backend emits as static data name
    // types with the same encoding the function symbols use.
    [[nodiscard]] BACKENDCORE_EXPORT std::string MangleNominal ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                                 MiddleEnd::TypeSystem::NominalId Id );

} // namespace Backend

} // namespace Volt
