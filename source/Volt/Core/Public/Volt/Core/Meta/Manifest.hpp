#pragma once

// Manifest.hpp — preprocessor plumbing shared by the X-macro manifests
// (Nodes.inl, TokenKind.inl, PassList.inl, ...). Keeping VOLT_FOR_EACH here
// means the manifests themselves stay declarative: one line per entry.

#define VOLT_STRINGIFY_IMPL( X ) #X
#define VOLT_STRINGIFY( X )      VOLT_STRINGIFY_IMPL( X )

#define VOLT_CONCAT_IMPL( A, B ) A##B
#define VOLT_CONCAT( A, B )      VOLT_CONCAT_IMPL( A, B )

#define VOLT_EXPAND( X ) X

// VOLT_FOR_EACH( Macro, ... ) expands to `Macro(a), Macro(b), ...` for up to
// 20 comma-separated arguments (comma-joined, so it drops cleanly into a
// braced initialiser or argument list).

#define VOLT_FE_1( M, X )        M( X )
#define VOLT_FE_2( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_1( M, __VA_ARGS__ ) )
#define VOLT_FE_3( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_2( M, __VA_ARGS__ ) )
#define VOLT_FE_4( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_3( M, __VA_ARGS__ ) )
#define VOLT_FE_5( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_4( M, __VA_ARGS__ ) )
#define VOLT_FE_6( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_5( M, __VA_ARGS__ ) )
#define VOLT_FE_7( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_6( M, __VA_ARGS__ ) )
#define VOLT_FE_8( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_7( M, __VA_ARGS__ ) )
#define VOLT_FE_9( M, X, ... )   M( X ), VOLT_EXPAND( VOLT_FE_8( M, __VA_ARGS__ ) )
#define VOLT_FE_10( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_9( M, __VA_ARGS__ ) )
#define VOLT_FE_11( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_10( M, __VA_ARGS__ ) )
#define VOLT_FE_12( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_11( M, __VA_ARGS__ ) )
#define VOLT_FE_13( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_12( M, __VA_ARGS__ ) )
#define VOLT_FE_14( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_13( M, __VA_ARGS__ ) )
#define VOLT_FE_15( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_14( M, __VA_ARGS__ ) )
#define VOLT_FE_16( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_15( M, __VA_ARGS__ ) )
#define VOLT_FE_17( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_16( M, __VA_ARGS__ ) )
#define VOLT_FE_18( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_17( M, __VA_ARGS__ ) )
#define VOLT_FE_19( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_18( M, __VA_ARGS__ ) )
#define VOLT_FE_20( M, X, ... )  M( X ), VOLT_EXPAND( VOLT_FE_19( M, __VA_ARGS__ ) )

// clang-format off
#define VOLT_FE_PICK( \
    _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
    _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
    NAME, ... ) NAME
// clang-format on

#define VOLT_FOR_EACH( M, ... )                                                                                                   \
    VOLT_EXPAND( VOLT_FE_PICK( __VA_ARGS__, VOLT_FE_20, VOLT_FE_19, VOLT_FE_18, VOLT_FE_17, VOLT_FE_16, VOLT_FE_15, VOLT_FE_14,    \
                               VOLT_FE_13, VOLT_FE_12, VOLT_FE_11, VOLT_FE_10, VOLT_FE_9, VOLT_FE_8, VOLT_FE_7, VOLT_FE_6,         \
                               VOLT_FE_5, VOLT_FE_4, VOLT_FE_3, VOLT_FE_2, VOLT_FE_1 )( M, __VA_ARGS__ ) )
