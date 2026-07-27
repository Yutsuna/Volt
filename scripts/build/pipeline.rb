# frozen_string_literal: true

require 'etc'
require 'fileutils'
require 'json'

module Volt::Build
  class Pipeline
    FEATURE_BIN_NAMES = {
      'VOLT_ENABLE_ASAN'    => 'Volt_asan',
      'VOLT_ENABLE_TSAN'    => 'Volt_tsan',
      'VOLT_ENABLE_UBSAN'   => 'Volt_ubsan',
      'VOLT_ENABLE_TESTING' => 'Volt_test'
    }.freeze

    def initialize( options, threads_per_job: SystemInfo.total_cpus )
      @options         = options
      @variant_name    = options[ :variant_name ] || determine_variant_name
      @threads_per_job = threads_per_job
      @build_dir       = File.join( 'build', @variant_name )
      @binary_path     = determine_binary_path
      @cache           = Cache.new( @options[ :build_type ], @options[ :cmake_flags ], @binary_path, @build_dir )
    end

    def execute!
      setup_cache_env!
      steps.each { |step| break if send( step ) == :halt }
    end

    private

    def setup_cache_env!
      ENV[ 'CCACHE_SLOPPINESS' ] ||= 'pch_defines,time_macros,file_stat_matches'
      ENV[ 'CCACHE_COMPRESS' ]   ||= '1'
    end

    def steps
      return [ :clean ] if @options[ :clean ]

      pipeline_steps = []

      if @options[ :actions ].any? { |a| %i[build format tidy test run].include?( a ) }
        pipeline_steps.concat( [ :configure, :build, :persist ] )
      end

      pipeline_steps << :format if @options[ :actions ].include?( :format )
      pipeline_steps << :tidy   if @options[ :actions ].include?( :tidy )
      pipeline_steps << :epilogue
      pipeline_steps.uniq
    end

    def clean
      Logger.info "Cleaning build directory...", prefix: @variant_name
      FileUtils.rm_rf( 'build' )
      :halt
    end

    def format
      run_tool 'format', "Formatting codebase with clang-format (incremental)..."
    end

    def tidy
      run_tool 'tidy', "Linting codebase with clang-tidy (parallel, incremental)..."
    end

    def run_tool( target, message )
      Logger.info message, prefix: @variant_name
      system( 'cmake', '--build', @build_dir, '--target', target, '-j', @threads_per_job.to_s ) or Logger.fatal!( "Target '#{target}' failed.", prefix: @variant_name )
      Logger.ok "#{target.capitalize} completed.", prefix: @variant_name
      :continue
    end

    def configure
      return :continue if @cache.valid?

      Logger.info "Configuring CMake (Mode: #{@options[:build_type]}, Variant: #{@variant_name})...", prefix: @variant_name
      FileUtils.mkdir_p( @build_dir )

      cc  = ENV[ 'CC' ] || 'gcc'
      cxx = ENV[ 'CXX' ] || 'g++'
      definitions = @options[ :cmake_flags ].map { |k, v| "-D#{k}=#{v}" }.join( ' ' )
      cmd = "cmake -B #{@build_dir} -G Ninja -DCMAKE_C_COMPILER=#{cc} -DCMAKE_CXX_COMPILER=#{cxx} -DCMAKE_BUILD_TYPE=#{@options[:build_type]} #{definitions}"

      system( cmd ) or Logger.fatal!( "CMake configuration failed.", prefix: @variant_name )
      @cache.save_configure!
      :continue
    end

    def build
      Logger.info "Building targets (#{@variant_name}, #{@threads_per_job} threads)...", prefix: @variant_name
      system( "cmake --build #{@build_dir} -j #{@threads_per_job}" ) ? :continue : Logger.fatal!( "Build failed.", prefix: @variant_name )
    end

    def persist
      FileUtils.mkdir_p( @build_dir )
      @cache.save!
      update_symlinks
      Logger.ok "Build completed.", prefix: @variant_name
      :continue
    end

    def update_symlinks
      FileUtils.mkdir_p( 'build/bin' )

      bin_name = File.basename( @binary_path )
      target_bin = File.join( '..', @variant_name, 'bin', bin_name )

      create_symlink( target_bin, File.join( 'build', 'bin', bin_name ) )

      active_feature = FEATURE_BIN_NAMES.keys.find { |var| @options[ :cmake_flags ][ var ] == 'ON' }
      if active_feature
        create_symlink( target_bin, File.join( 'build', 'bin', FEATURE_BIN_NAMES[ active_feature ] ) )
      end

      src_compile_cmds = File.join( @build_dir, 'compile_commands.json' )
      if File.exist?( src_compile_cmds )
        create_symlink( File.join( @variant_name, 'compile_commands.json' ), 'build/compile_commands.json' )
      end
    end

    def create_symlink( source, target )
      FileUtils.rm_f( target )
      File.symlink( source, target )
    rescue StandardError => e
      Logger.warn "Symlink creation failed for #{target}: #{e.message}", prefix: @variant_name
    end

    def epilogue
      test    if @options[ :actions ].include?( :test )
      run_bin if @options[ :run ]
      :halt
    end

    def test
      jobs = @threads_per_job
      Logger.info "Running test suites (ctest, #{jobs} jobs)...", prefix: @variant_name
      system( 'ctest', '--test-dir', @build_dir, '--output-on-failure', '--parallel', jobs.to_s ) or Logger.fatal!( "Tests failed.", prefix: @variant_name )
      Logger.ok "All tests passed.", prefix: @variant_name
    end

    def run_bin
      Logger.fatal!( "Executable not found at #{@binary_path}", prefix: @variant_name ) unless File.exist?( @binary_path )

      Logger.info "Executing: #{@binary_path} #{@options[:run_args].join(' ')}", prefix: @variant_name
      puts "--------------------------------------------------"
      exec( @binary_path, *@options[ :run_args ] )
    end

    def determine_variant_name
      parts = [ @options[ :build_type ].downcase ]
      Options::FLAGS_MAP.each do |flag_name, var_name|
        parts << flag_name if @options[ :cmake_flags ][ var_name ] == 'ON'
      end
      parts.join( '-' )
    end

    def determine_binary_path
      postfix = @options[ :build_type ] == 'Debug' ? '_d' : ''
      File.join( @build_dir, 'bin', "volt#{postfix}" )
    end
  end
end
