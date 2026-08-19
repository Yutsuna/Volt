#ifndef VOLT_UNIT
    #define VOLT_UNIT( Suffix, Family, Multiplier, IsFloat )
#endif

// Ordered longest-suffix first so greedy prefix matching always chooses the most specific unit.

// --- 4 chars ---
VOLT_UNIT( "kbps", Rate, 1000ULL, false )
VOLT_UNIT( "Mbps", Rate, 1000000ULL, false )
VOLT_UNIT( "Gbps", Rate, 1000000000ULL, false )
VOLT_UNIT( "turn", Angle, ( 2.0 * 3.141592653589793238462643383279502884 ), true )

// --- 3 chars / bytes ---
VOLT_UNIT( "KiB", DataSize, 1024ULL, false )
VOLT_UNIT( "MiB", DataSize, 1048576ULL, false )
VOLT_UNIT( "GiB", DataSize, 1073741824ULL, false )
VOLT_UNIT( "TiB", DataSize, 1099511627776ULL, false )
VOLT_UNIT( "PiB", DataSize, 1125899906842624ULL, false )
VOLT_UNIT( "EiB", DataSize, 1152921504606846976ULL, false )
VOLT_UNIT( "kHz", Freq, 1000ULL, false )
VOLT_UNIT( "MHz", Freq, 1000000ULL, false )
VOLT_UNIT( "GHz", Freq, 1000000000ULL, false )
VOLT_UNIT( "bps", Rate, 1ULL, false )
VOLT_UNIT( "deg", Angle, ( 3.141592653589793238462643383279502884 / 180.0 ), true )
VOLT_UNIT( "rad", Angle, 1.0, true )
VOLT_UNIT( "\xC2\xB5s", Duration, ( 1.0 / 1000.0 ), true )
VOLT_UNIT( "\xCE\xBCs", Duration, ( 1.0 / 1000.0 ), true )

// --- 2 chars ---
VOLT_UNIT( "KB", DataSize, 1000ULL, false )
VOLT_UNIT( "Ko", DataSize, 1000ULL, false )
VOLT_UNIT( "MB", DataSize, 1000000ULL, false )
VOLT_UNIT( "Mo", DataSize, 1000000ULL, false )
VOLT_UNIT( "GB", DataSize, 1000000000ULL, false )
VOLT_UNIT( "Go", DataSize, 1000000000ULL, false )
VOLT_UNIT( "TB", DataSize, 1000000000000ULL, false )
VOLT_UNIT( "To", DataSize, 1000000000000ULL, false )
VOLT_UNIT( "PB", DataSize, 1000000000000000ULL, false )
VOLT_UNIT( "Po", DataSize, 1000000000000000ULL, false )
VOLT_UNIT( "EB", DataSize, 1000000000000000000ULL, false )
VOLT_UNIT( "Eo", DataSize, 1000000000000000000ULL, false )
VOLT_UNIT( "ms", Duration, 1ULL, false )
VOLT_UNIT( "us", Duration, ( 1.0 / 1000.0 ), true )
VOLT_UNIT( "ns", Duration, ( 1.0 / 1000000.0 ), true )
VOLT_UNIT( "Hz", Freq, 1ULL, false )

// --- 1 char ---
VOLT_UNIT( "B", DataSize, 1ULL, false )
VOLT_UNIT( "s", Duration, 1000ULL, false )
VOLT_UNIT( "m", Duration, 60000ULL, false )
VOLT_UNIT( "h", Duration, 3600000ULL, false )
VOLT_UNIT( "d", Duration, 86400000ULL, false )
VOLT_UNIT( "%", Ratio, ( 1.0 / 100.0 ), true )

#undef VOLT_UNIT
