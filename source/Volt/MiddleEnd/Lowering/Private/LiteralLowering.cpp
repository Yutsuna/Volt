#include "Volt/MiddleEnd/Lowering/LoweringPasses.hpp"

#include "DeclStmtWalker.hpp"
#include "MemberResolver.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

void Volt::MiddleEnd::Lowering::LowerArrayLit ( TypeCheckerContext &Context,
                                                Frontend::ExprId Id,
                                                Frontend::ExprList Elements,
                                                SemaTypeId LiteralType )
{
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // The receiver's own SemaTypeId is already known — the literal's own
    // type, already instantiated with its concrete element type — so the
    // Object child is stamped directly rather than rebuilt from a
    // source-level GenericInst. LookupOn/MemberType still perform the
    // ordinary member resolution for `new`; only the receiver's *type* comes
    // from Sema's own value rather than re-deriving it from a name.
    const NominalId Base            = Context.Ctx.Values.Get( LiteralType ).Base;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId CtorId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );

    // `tmp = T.new()` — an ordinary implicit local declaration (Assign onto
    // an Identifier ScopeResolver never bound), exactly the shape `arr =
    // Array.new()` written by hand takes. TypeCheckerContext::WriteLocal
    // falls back to the flat, name-keyed Locals map for exactly this case —
    // "a node minted after Order 10" (TypeCheckerContext.cpp).
    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__array_lit" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TmpTarget, .Value = CtorId } } );

    ScopeId CurrentScope = Context.Ctx.Scopes.ScopeOfExpr( Id );
    if ( not CurrentScope.IsValid() )
    {
        CurrentScope = ScopeId{ 0 };
    }
    Context.Ctx.Scopes.Declare( CurrentScope, TmpName, TmpTarget );
    const MiddleEnd::Resolver::Binding *Bound = Context.Ctx.Scopes.Resolve( CurrentScope, TmpName );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TmpTarget, *Bound, false );
    }

    Frontend::StmtList Body;
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AssignId } } ) );

    for ( const Frontend::ExprId Elem : Elements )
    {
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        if ( Bound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *Bound, true );
        }
        const Frontend::ExprId AppendId = Ast.Add(
            Frontend::ExprNode{ Frontend::Binary{ .Loc = {}, .Op = Frontend::TokenKind::Shl, .Lhs = TmpUse, .Rhs = Elem } } );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AppendId } } ) );
    }

    const Frontend::ExprId TrailingUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TrailingUse, *Bound, true );
    }
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TrailingUse } } ) );

    // Type every synthesized statement off the local `Body` copy — never off
    // a re-read of the arena slot at `Id`, which WalkStmt's own Add() calls
    // (constructor/operator resolution, generic instantiation) could
    // reallocate out from under a reference (rules/ast-rewrite.md).
    for ( const Frontend::StmtId StmtId : Body )
    {
        WalkStmt( Context, StmtId );
    }

    // Copy-out (Elements, above) / compute (WalkStmt, above) / write-back:
    // the arena slot is assigned only now, after every Add() this rewrite
    // performs has already happened.
    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::BeginExpr{ .Loc = {}, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = {} } };

    // This slot now *is* a construction, but nothing about its shape says so
    // any more — a `BeginExpr` ending on a name reads as a place, and the
    // `bConstructs` a caller would look for sits on the inner `T.new` call
    // rather than here. Say it, so the surrounding region releases what this
    // just built (`for x in [ 10, 20, 30 ]` otherwise strands its array).
    Context.OwnedExprSites.insert( Id.Value );
}

void Volt::MiddleEnd::Lowering::LowerArrayLits ( TypeCheckerContext &Context )
{
    // Bound before the first Add() — every node this rewrite creates lands
    // past OriginalCount and is never itself an ArrayLit, so skipping it is
    // deliberate (rules/ast-rewrite.md). A nested literal was parsed first,
    // so it always carries a smaller index than the one containing it: the
    // sweep processes it — and rewrites it into a BeginExpr — before the
    // outer literal's own turn, exactly the innermost-first order other
    // lowerings rely on.
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        // A metadata expression (an `@[...]` annotation argument, a macro
        // invocation's own arguments) is never evaluated and never typed —
        // InferExpr short-circuits it on sight. Lowering it into runtime
        // construction code would build something nothing reads.
        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::ArrayLit>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        // Copied out before Add(): std::get, like std::visit, would otherwise
        // hand back a reference straight into the arena slot this same call
        // goes on to rewrite (rules/ast-rewrite.md).
        const Frontend::ExprList Elements = std::get<Frontend::ArrayLit>( Context.Ctx.Ast.Expr( Id ) ).Elements;

        LowerArrayLit( Context, Id, Elements, Context.Ctx.Values.ExprType( Id ) );
    }
}

void Volt::MiddleEnd::Lowering::LowerHashLit (
    TypeCheckerContext &Context, Frontend::ExprId Id, Frontend::ExprList Keys, Frontend::ExprList Values, SemaTypeId LiteralType )
{
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const NominalId Base            = Context.Ctx.Values.Get( LiteralType ).Base;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId CtorId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );

    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__hash_lit" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TmpTarget, .Value = CtorId } } );

    ScopeId CurrentScope = Context.Ctx.Scopes.ScopeOfExpr( Id );
    if ( not CurrentScope.IsValid() )
    {
        CurrentScope = ScopeId{ 0 };
    }
    Context.Ctx.Scopes.Declare( CurrentScope, TmpName, TmpTarget );
    const MiddleEnd::Resolver::Binding *Bound = Context.Ctx.Scopes.Resolve( CurrentScope, TmpName );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TmpTarget, *Bound, false );
    }

    Frontend::StmtList Body;
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AssignId } } ) );

    const Frontend::Symbol SetOpSym = Ast.Strings().Intern( "[]=" );
    const std::size_t PairCount     = std::min( Keys.Size(), Values.Size() );

    for ( std::size_t Index = 0; Index < PairCount; ++Index )
    {
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        if ( Bound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *Bound, true );
        }
        const Frontend::ExprId SetMemberId =
            Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = TmpUse, .Name = SetOpSym } } );

        Frontend::ExprList Args;
        Args.PushBack( Keys[Index] );
        Args.PushBack( Values[Index] );

        Frontend::SymbolList ArgNames;
        ArgNames.PushBack( Volt::Core::Symbol{} );
        ArgNames.PushBack( Volt::Core::Symbol{} );

        const Frontend::ExprId IndexSetCallId = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = {}, .Callee = SetMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = IndexSetCallId } } ) );
    }

    const Frontend::ExprId TrailingUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TrailingUse, *Bound, true );
    }
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TrailingUse } } ) );

    for ( const Frontend::StmtId StmtId : Body )
    {
        WalkStmt( Context, StmtId );
    }

    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::BeginExpr{ .Loc = {}, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = {} } };

    // Same fact, same reason as `LowerArrayLit`'s own note above.
    Context.OwnedExprSites.insert( Id.Value );
}

void Volt::MiddleEnd::Lowering::LowerHashLits ( TypeCheckerContext &Context )
{
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::HashLit>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        const Frontend::HashLit HashLitNode = std::get<Frontend::HashLit>( Context.Ctx.Ast.Expr( Id ) );
        const Frontend::ExprList Keys       = HashLitNode.Keys;
        const Frontend::ExprList Values     = HashLitNode.Values;

        LowerHashLit( Context, Id, Keys, Values, Context.Ctx.Values.ExprType( Id ) );
    }
}

[[nodiscard]] static constexpr int HexVal ( char C ) noexcept
{
    if ( C >= '0' and C <= '9' )
    {
        return C - '0';
    }
    if ( C >= 'a' and C <= 'f' )
    {
        return C - 'a' + 10;
    }
    if ( C >= 'A' and C <= 'F' )
    {
        return C - 'A' + 10;
    }
    return -1;
}

static std::string DecodeStringText ( std::string_view Text )
{
    std::string Out;
    Out.reserve( Text.size() );
    for ( std::size_t Index = 0; Index < Text.size(); ++Index )
    {
        if ( Text[Index] == '\\' and Index + 1 < Text.size() )
        {
            const char Next = Text[Index + 1];
            if ( ( Next == 'x' or Next == 'X' ) and Index + 2 < Text.size() )
            {
                const int H1 = HexVal( Text[Index + 2] );
                if ( H1 >= 0 )
                {
                    int Val              = H1;
                    std::size_t Advanced = 2;
                    if ( Index + 3 < Text.size() )
                    {
                        const int H2 = HexVal( Text[Index + 3] );
                        if ( H2 >= 0 )
                        {
                            Val      = ( Val << 4 ) | H2;
                            Advanced = 3;
                        }
                    }
                    Out.push_back( static_cast<char>( Val ) );
                    Index += Advanced;
                    continue;
                }
            }

            switch ( Next )
            {
            case 'a':
                Out.push_back( '\a' );
                ++Index;
                continue;
            case 'b':
                Out.push_back( '\b' );
                ++Index;
                continue;
            case 'n':
                Out.push_back( '\n' );
                ++Index;
                continue;
            case 'r':
                Out.push_back( '\r' );
                ++Index;
                continue;
            case 't':
                Out.push_back( '\t' );
                ++Index;
                continue;
            case 'v':
                Out.push_back( '\v' );
                ++Index;
                continue;
            case 'f':
                Out.push_back( '\f' );
                ++Index;
                continue;
            case '0':
                Out.push_back( '\0' );
                ++Index;
                continue;
            case 'e':
                Out.push_back( '\x1b' );
                ++Index;
                continue;
            case '\\':
                Out.push_back( '\\' );
                ++Index;
                continue;
            case '"':
                Out.push_back( '"' );
                ++Index;
                continue;
            case '\'':
                Out.push_back( '\'' );
                ++Index;
                continue;
            default:
                break;
            }
        }
        Out.push_back( Text[Index] );
    }
    return Out;
}

void Volt::MiddleEnd::Lowering::LowerStringLit ( TypeCheckerContext &Context,
                                                 Frontend::ExprId Id,
                                                 Frontend::Symbol ValueSym,
                                                 SemaTypeId LiteralType )
{
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const NominalId Base            = Context.Ctx.Values.Get( LiteralType ).Base;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Resolution InitRes = LookupOn( Context, LiteralType, "initialize" );
    SemaTypeId DataParamType;
    SemaTypeId SizeParamType;
    if ( InitRes.Decl != nullptr and InitRes.Params.Size() >= 2 )
    {
        DataParamType = InitRes.Params[0];
        SizeParamType = InitRes.Params[1];
    }

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );

    const Frontend::ExprId RawBytesId = Ast.Add( Frontend::ExprNode{ Frontend::StringLiteral{ .Loc = {}, .Value = ValueSym } } );
    if ( DataParamType.IsValid() )
    {
        Context.Ctx.Values.SetExprType( RawBytesId, DataParamType );
    }

    const std::string DecodedText = DecodeStringText( Ast.Text( ValueSym ) );
    const std::string LenStr      = std::to_string( DecodedText.size() );
    const Frontend::Symbol LenSym = Ast.Strings().Intern( LenStr );
    const Frontend::ExprId LenId  = Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = {}, .Raw = LenSym } } );
    if ( SizeParamType.IsValid() )
    {
        Context.Ctx.Values.SetExprType( LenId, SizeParamType );
    }

    Frontend::ExprList Args;
    Args.PushBack( RawBytesId );
    Args.PushBack( LenId );

    Frontend::SymbolList ArgNames;
    ArgNames.PushBack( Volt::Core::Symbol{} );
    ArgNames.PushBack( Volt::Core::Symbol{} );

    const Frontend::ExprId CtorCallId = Ast.Add( Frontend::ExprNode{
        Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

    InferExpr( Context, CtorCallId );

    if ( const auto Entry = Context.CalleeResolution.find( CtorCallId.Value ); Entry != Context.CalleeResolution.end() )
    {
        Context.CalleeResolution[Id.Value] = Entry->second;
    }
    if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
    {
        Context.Ctx.Values.SetExprType( Id, CallTypeRes );
    }

    Ast.Expr( Id ) = Ast.Expr( CtorCallId );
}

void Volt::MiddleEnd::Lowering::LowerStringLits ( TypeCheckerContext &Context )
{
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::StringLiteral>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        const Frontend::Symbol ValueSym = std::get<Frontend::StringLiteral>( Context.Ctx.Ast.Expr( Id ) ).Value;
        LowerStringLit( Context, Id, ValueSym, Context.Ctx.Values.ExprType( Id ) );
    }
}

void Volt::MiddleEnd::Lowering::LowerTypeOfExpr ( TypeCheckerContext &Context, Frontend::ExprId Id, SemaTypeId InferredType )
{
    const auto TypeBase = Context.Ctx.Types.LookupNodeKind( "TypeOfExpr" );
    if ( not TypeBase )
    {
        return;
    }

    Frontend::AstContext &Ast        = Context.Ctx.Ast;
    const SemaTypeId TypeNominalType = Context.MakeType( *TypeBase, {} );

    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( *TypeBase ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, TypeNominalType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );

    std::string TypeDesc;
    if ( InferredType.IsValid() and Context.Ctx.Values.Has( InferredType ) )
    {
        TypeDesc = Context.Ctx.Types.Universe().Describe( Context.Ctx.Types, InferredType );
    }
    else
    {
        TypeDesc = "<unknown>";
    }

    const Frontend::Symbol DescSym     = Ast.Strings().Intern( TypeDesc );
    const Frontend::ExprId StringLitId = Ast.Add( Frontend::ExprNode{ Frontend::StringLiteral{ .Loc = {}, .Value = DescSym } } );

    Frontend::ExprList Args;
    Args.PushBack( StringLitId );

    Frontend::SymbolList ArgNames;
    ArgNames.PushBack( Volt::Core::Symbol{} );

    const Frontend::ExprId CtorCallId = Ast.Add( Frontend::ExprNode{
        Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

    InferExpr( Context, CtorCallId );

    if ( const auto Entry = Context.CalleeResolution.find( CtorCallId.Value ); Entry != Context.CalleeResolution.end() )
    {
        Context.CalleeResolution[Id.Value] = Entry->second;
    }
    if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
    {
        Context.Ctx.Values.SetExprType( Id, CallTypeRes );
    }

    Ast.Expr( Id ) = Ast.Expr( CtorCallId );
    Context.OwnedExprSites.insert( Id.Value );
}

void Volt::MiddleEnd::Lowering::LowerTypeOfExprs ( TypeCheckerContext &Context )
{
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::TypeOfExpr>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        const SemaTypeId InferredType = Context.Ctx.Values.SiteType( BindingSite{ Id } );
        LowerTypeOfExpr( Context, Id, InferredType );
    }
}
