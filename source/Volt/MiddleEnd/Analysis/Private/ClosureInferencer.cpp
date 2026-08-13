#include "ClosureInferencer.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

#include "DeclStmtWalker.hpp"
#include "ExprInferencer.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

Volt::Core::SmallVec<Volt::MiddleEnd::TypeSystem::SemaTypeId, 2>
Volt::MiddleEnd::Analysis::BindClosureParams ( TypeCheckerContext &Context, const Frontend::ParamList &Params )
{
    // The `&block : T -> U` slot the enclosing call expects, if any. Copied
    // rather than referenced: ResolveTypeExpr interns into Values below and
    // may move the arena. Consumed here and cleared, so a closure nested
    // deeper in this one's body cannot pick it up a second time.
    Volt::Core::SmallVec<SemaTypeId, 2> Expected;
    if ( Context.Ctx.Values.Has( Context.ExpectedClosure ) )
    {
        Expected = Context.Ctx.Values.Get( Context.ExpectedClosure ).Args;
    }
    Context.ExpectedClosure = SemaTypeId{};

    UnitSink Sink = Context.MakeSink();
    Volt::Core::SmallVec<SemaTypeId, 2> Types;
    for ( std::size_t Index = 0; Index < Params.Size(); ++Index )
    {
        const Frontend::ParamId PId  = Params[Index];
        const Frontend::Param &Entry = Context.Ctx.Ast.GetParam( PId );
        SemaTypeId ParamType = ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Entry.DeclType );

        // Unannotated, so take the type from the slot: a callable's
        // arguments are its result followed by its parameters, hence the
        // 1 + Index. `arr.each do | i |` types `i` here and nowhere else.
        if ( not ParamType.IsValid() and 1 + Index < Expected.Size() )
        {
            ParamType = Expected[1 + Index];
        }

        Context.LocalTypes[BindingSite{ PId }] = ParamType;
        Context.LocalSites[Entry.Name]         = BindingSite{ PId };
        Context.Locals[Entry.Name]             = ParamType;
        Context.Ctx.Values.SetSiteType( BindingSite{ PId }, ParamType );
        if ( Entry.Default.IsValid() )
        {
            static_cast<void>( InferExpr( Context, Entry.Default ) );
        }
        Types.PushBack( ParamType );
    }
    return Types;
}

Volt::MiddleEnd::TypeSystem::SemaTypeId Volt::MiddleEnd::Analysis::TrailingType ( TypeCheckerContext &Context,
                                                                                  Frontend::StmtList Body )
{
    SemaTypeId Last;
    for ( const Frontend::StmtId Id : Body )
    {
        WalkStmt( Context, Id );
        Last = SemaTypeId{};
        if ( const auto *Stmt = std::get_if<Frontend::ExprStmt>( &Context.Ctx.Ast.Stmt( Id ) ) )
        {
            Last = Context.Ctx.Values.ExprType( Stmt->Expr );
        }
    }
    return Last;
}

Volt::MiddleEnd::TypeSystem::SemaTypeId
Volt::MiddleEnd::Analysis::ClosureType ( TypeCheckerContext &Context,
                                         std::string_view NodeKind,
                                         Volt::MiddleEnd::TypeSystem::SemaTypeId Result,
                                         const Volt::Core::SmallVec<Volt::MiddleEnd::TypeSystem::SemaTypeId, 2> &Params )
{
    const auto Base = Context.Ctx.Types.LookupNodeKind( NodeKind );
    if ( not Base )
    {
        return SemaTypeId{};
    }

    Volt::Core::SmallVec<SemaTypeId, 2> Args;
    Args.PushBack( Result );
    for ( const SemaTypeId Param : Params )
    {
        Args.PushBack( Param );
    }
    return Context.MakeType( *Base, std::move( Args ) );
}
