// SymbolNameEmitter.cpp — `_V_symbol_name`, the table behind `Symbol#to_string`.
//
// Synthesised for the same reason `_V_init_all` is: it is the *build* that
// knows every `:symbol` written anywhere in it, and no Volt body can be handed
// that fact. What it deliberately does not do is decide anything about strings
// — it hands back a NUL-terminated byte pointer, and turning that into a
// `String` is `Symbol#to_string`'s own line of Volt (rules/zero-hardcode.md).
// No type name and no method name enters C++ here.
//
// A switch rather than a sorted table plus a search loop: the keys are compile
// -time constants, so LLVM already knows how to pick between a jump table, a
// bit test and a binary search, and choosing for it here would be a worse
// version of a decision the target machine owns.

#include "Functions/FunctionRegistry.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/BackendCore/SymbolRegistry.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

bool Volt::Backend::Llvm::EmitSymbolNames ( EmitterServices &Services )
{
    llvm::LLVMContext &Context = Services.Ctx->Context();

    // Under JIT execution (PerUnit modules or in-process JIT main), the host
    // runtime in BackendCore exports `_V_symbol_name` dynamically. Emitting a
    // static switch here would freeze the symbol table to startup units and
    // shadow the dynamic host runtime function.
    if ( PerUnitModules( Services ) or Services.Options->EntrySymbol == "__volt_jit_main" )
    {
        return true;
    }

    // The name the stdlib's `@[External( "volt", "_V_symbol_name" )]` declares.
    // Absent means no stdlib declared it — a tooling build, or a build with
    // `--no-stdlib` — and there is then nothing that could call it.
    llvm::Function *NameFn = Services.Functions->Find( "_V_symbol_name" );
    if ( NameFn == nullptr or not NameFn->empty() or Services.Build == nullptr )
    {
        return true;
    }

    // The shape is the stdlib declaration's, read back rather than assumed: a
    // switch needs an integer to dispatch on, and the caller needs the pointer
    // the Volt signature promised. Both are facts about that declaration.
    if ( NameFn->arg_size() != 1 or not NameFn->getArg( 0 )->getType()->isIntegerTy() or
         not NameFn->getReturnType()->isPointerTy() )
    {
        static_cast<void>( Services.Diag->Fail( "llvm: '_V_symbol_name' is declared with a shape this backend cannot "
                                                "define — one integer parameter returning a pointer is what it fills in" ) );
        return false;
    }

    std::string Clash;
    const std::vector<FSymbolEntry> Table = CollectSymbols( *Services.Build, Clash );
    if ( not Clash.empty() )
    {
        static_cast<void>( Services.Diag->Fail( "llvm: the symbols ':" + Clash +
                                                "' hash to one runtime identity, so they would compare equal — rename one" ) );
        return false;
    }

    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", NameFn ) };

    // The miss arm. A `Symbol` value that is in no unit's AST cannot be
    // produced by any literal, so this is unreachable in a well-formed program;
    // returning the empty string rather than trapping keeps `to_string` total,
    // which is what its signature promises.
    llvm::BasicBlock *Miss = llvm::BasicBlock::Create( Context, "sym.miss", NameFn );

    llvm::Argument *Value = NameFn->getArg( 0 );
    Value->setName( "sym.value" );
    auto *ValueTy = llvm::cast<llvm::IntegerType>( Value->getType() );

    llvm::SwitchInst *Switch = Shell.CreateSwitch( Value, Miss, static_cast<unsigned>( Table.size() ) );

    for ( const FSymbolEntry &Entry : Table )
    {
        const std::string Name{ Entry.Name };
        llvm::BasicBlock *Hit = llvm::BasicBlock::Create( Context, "sym." + Name, NameFn );
        Switch->addCase( llvm::ConstantInt::get( ValueTy, Entry.Value ), Hit );

        llvm::IRBuilder<> HitShell{ Hit };
        static_cast<void>( HitShell.CreateRet( HitShell.CreateGlobalString( Name, ".sym", 0, Services.Ctx->ModPtr() ) ) );
    }

    llvm::IRBuilder<> MissShell{ Miss };
    static_cast<void>( MissShell.CreateRet( MissShell.CreateGlobalString( "", ".sym", 0, Services.Ctx->ModPtr() ) ) );

    return true;
}
