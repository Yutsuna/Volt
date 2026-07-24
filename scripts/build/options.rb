# frozen_string_literal: true

require 'set'

module Volt::Build
  class Options
    BUILD_TYPES   = %w[debug release].freeze
    TOOL_ACTIONS  = %w[format tidy test].freeze
    FLAGS_MAP     = {
      'asan'    => 'VOLT_ENABLE_ASAN',
      'ubsan'   => 'VOLT_ENABLE_UBSAN',
      'tsan'    => 'VOLT_ENABLE_TSAN',
      'testing' => 'VOLT_ENABLE_TESTING',
      'unity'   => 'VOLT_UNITY_BUILD',
      'llvm'    => 'VOLT_ENABLE_LLVM'
    }.freeze
    SANITIZERS    = %w[VOLT_ENABLE_ASAN VOLT_ENABLE_TSAN VOLT_ENABLE_UBSAN].freeze

    def self.parse( args = ARGV )
      args_copy = args.dup
      run_args = []

      if ( dash_idx = args_copy.index( '--' ) )
        run_args = args_copy[ ( dash_idx + 1 )..-1 ] || []
        args_copy = args_copy[ 0...dash_idx ]
      end

      return { help: true } if ( args_copy & %w[help -h --help] ).any?

      jobs = []
      clean_first = false
      has_explicit_token = false
      current_job = new_job_spec

      until args_copy.empty?
        arg = args_copy.shift.downcase

        if arg == 'clean'
          clean_first = true
          next
        end

        unless valid_token?( arg )
          puts "\x1b[31mError:\x1b[0m Unknown option or command '\x1b[1m#{arg}\x1b[0m'."
          puts "Run 'volt-build help' for usage instructions."
          exit 1
        end

        has_explicit_token = true

        if should_split_job?( current_job, arg )
          jobs << current_job
          current_job = new_job_spec
        end

        apply_token!( current_job, arg )
      end

      current_job[ :run_args ].concat( run_args )

      if clean_first && !has_explicit_token && !current_job[ :run ] && current_job[ :actions ] == Set.new( [ :build ] )
        return { only_clean: true, help: false }
      end

      jobs << current_job unless job_empty?( current_job ) && jobs.any?
      jobs = [ new_job_spec ] if jobs.empty?
      jobs.each { |j| j[ :run_args ].concat( run_args ) if j[ :run_args ].empty? }

      jobs.each { |j| finalize_job!( j ) }

      { jobs: jobs, clean_first: clean_first, help: false }
    end

    def self.valid_token?( arg )
      BUILD_TYPES.include?( arg ) || TOOL_ACTIONS.include?( arg ) || FLAGS_MAP.key?( arg ) || arg == 'run'
    end

    def self.show_usage
      puts <<~USAGE
        \x1b[1;33mVolt Build Tool\x1b[0m

        \x1b[1mUsage:\x1b[0m volt-build [options] [-- [run_args]]

        \x1b[1mCommands:\x1b[0m
          clean         Clean the build directory before building.
          format        Format all source files using clang-format (parallel, cached).
          tidy          Run parallel and cached clang-tidy on source files.
          test          Build then run the ctest suites in parallel (implies testing).
          help, -h      Show this help message.

        \x1b[1mBuild Types:\x1b[0m
          debug         Build in Debug mode (default, executable: Volt_d).
          release       Build in Release mode (executable: Volt).

        \x1b[1mSanitizers & Features:\x1b[0m
          asan          Enable AddressSanitizer (VOLT_ENABLE_ASAN=ON).
          ubsan         Enable UndefinedBehaviorSanitizer (VOLT_ENABLE_UBSAN=ON).
          tsan          Enable ThreadSanitizer (VOLT_ENABLE_TSAN=ON).
          testing       Enable Testing (VOLT_ENABLE_TESTING=ON).
          unity         Enable Unity Build (VOLT_UNITY_BUILD=ON).
          llvm          Build the native AOT backend (VOLT_ENABLE_LLVM=ON).

        \x1b[1mExecution:\x1b[0m
          run           Run the built binary after a successful build.
          --            Pass all subsequent arguments directly to the Volt binary.

        \x1b[1mExamples:\x1b[0m
          volt-build clean
          volt-build format tidy
          volt-build release run -- --help
          volt-build asan tsan ubsan
          volt-build debug release format
      USAGE
    end

    private

    def self.new_job_spec
      {
        build_type: 'Debug',
        run: false,
        actions: Set.new( [ :build ] ),
        cmake_flags: FLAGS_MAP.values.each_with_object( {} ) { |var, h| h[ var ] = 'OFF' },
        run_args: [],
        explicit_build_type: false
      }
    end

    def self.should_split_job?( job, token )
      return false if job_empty?( job )

      if BUILD_TYPES.include?( token )
        job[ :explicit_build_type ]
      elsif FLAGS_MAP.key?( token )
        var = FLAGS_MAP[ token ]
        SANITIZERS.include?( var ) && active_sanitizers( job ).any? { |s| s != var }
      else
        false
      end
    end

    def self.apply_token!( job, arg )
      case arg
      when *BUILD_TYPES
        job[ :build_type ] = arg.capitalize
        job[ :explicit_build_type ] = true
      when *TOOL_ACTIONS
        job[ :actions ] << arg.to_sym
        job[ :cmake_flags ][ FLAGS_MAP[ 'testing' ] ] = 'ON' if arg == 'test'
      when 'run'
        job[ :run ] = true
        job[ :actions ] << :run
      when *FLAGS_MAP.keys
        job[ :cmake_flags ][ FLAGS_MAP[ arg ] ] = 'ON'
      end
    end

    def self.finalize_job!( job )
      parts = [ job[ :build_type ].downcase ]
      FLAGS_MAP.each do |flag_name, var_name|
        parts << flag_name if job[ :cmake_flags ][ var_name ] == 'ON'
      end
      job[ :variant_name ] = parts.join( '-' )
    end

    def self.job_empty?( job )
      !job[ :explicit_build_type ] && active_flags( job ).empty? && job[ :actions ] == Set.new( [ :build ] ) && job[ :run_args ].empty?
    end

    def self.active_flags( job )
      job[ :cmake_flags ].select { |_, v| v == 'ON' }.keys
    end

    def self.active_sanitizers( job )
      active_flags( job ) & SANITIZERS
    end
  end
end
