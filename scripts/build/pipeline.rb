# frozen_string_literal: true

require 'etc'
require 'fileutils'
require 'json'


module Volt::Build


  class Pipeline

    def initialize( options )
      @options      = options
      @variant_name = determine_variant_name
      @build_dir    = File.join( 'build', @variant_name )
      @binary_path  = determine_binary_path
      @cache        = Cache.new( @options[ :build_type ], @options[ :cmake_flags ], @binary_path, @build_dir )
    end

    def execute!
      steps.each { |step| break if send( step ) == :halt }
    end

    private

    def steps
      return [ :clean ]               if @options[ :clean ]
      return [ :configure, :format ]  if @options[ :format ]
      return [ :configure, :tidy ]    if @options[ :tidy ]
      [ :validate, :configure, :build, :persist, :epilogue ]
    end

    def clean
      Logger.info "Cleaning build directory..."
      FileUtils.rm_rf( 'build' )
      :halt
    end

    def validate
      return :continue unless @cache.valid?

      Logger.ok "No changes detected for variant '#{@variant_name}'. Build is up-to-date (Cached)."
      update_symlinks
      epilogue
    end

    def format
      run_tool 'format', "Formatting codebase with clang-format (incremental)..."
    end

    def tidy
      run_tool 'tidy', "Linting codebase with clang-tidy (parallel, incremental)..."
    end

    def run_tool( target, message )
      Logger.info message
      system( 'cmake', '--build', @build_dir, '--target', target ) or Logger.fatal!( "Target '#{target}' failed." )
      Logger.ok "#{target.capitalize} completed."
      :halt
    end

    def configure
      return :continue unless configure_required?

      Logger.info "Configuring CMake (Mode: #{@options[:build_type]}, Variant: #{@variant_name})..."
      FileUtils.mkdir_p( @build_dir )

      definitions = @options[:cmake_flags].map { |k, v| "-D#{k}=#{v}" }.join(' ')
      cmd = "cmake -B #{@build_dir} -G Ninja -DCMAKE_BUILD_TYPE=#{@options[:build_type]} #{definitions}"

      system(cmd) or Logger.fatal!("CMake configuration failed.")
      @cache.save_configure!
      :continue
    end

    def build
      Logger.info "Building targets (#{@variant_name})..."
      system("cmake --build #{@build_dir}") ? :continue : Logger.fatal!("Build failed.")
    end

    def persist
      FileUtils.mkdir_p( @build_dir )
      @cache.save!
      update_symlinks
      Logger.ok "Build completed."
      :continue
    end

    def update_symlinks
      FileUtils.mkdir_p( 'build/bin' )

      bin_name = File.basename( @binary_path )
      target_bin = File.join( '..', @variant_name, 'bin', bin_name )
      symlink_bin = File.join( 'build', 'bin', bin_name )

      FileUtils.rm_f( symlink_bin )
      File.symlink( target_bin, symlink_bin ) rescue nil

      src_compile_cmds = File.join( @build_dir, 'compile_commands.json' )
      if File.exist?( src_compile_cmds )
        target_cmds = File.join( @variant_name, 'compile_commands.json' )
        symlink_cmds = 'build/compile_commands.json'
        FileUtils.rm_f( symlink_cmds )
        File.symlink( target_cmds, symlink_cmds ) rescue nil
      end
    end

    def epilogue
      test    if @options[ :test ]
      run_bin if @options[ :run ]
      :halt
    end

    def test
      jobs = Etc.nprocessors
      Logger.info "Running test suites (ctest, #{jobs} jobs)..."
      system( 'ctest', '--test-dir', @build_dir, '--output-on-failure', '--parallel', jobs.to_s ) or Logger.fatal!( "Tests failed." )
      Logger.ok "All tests passed."
    end

    def run_bin
      Logger.fatal!("Executable not found at #{@binary_path}") unless File.exist?(@binary_path)

      Logger.info "Executing: #{@binary_path} #{@options[:run_args].join(' ')}"
      puts "--------------------------------------------------"
      exec(@binary_path, *@options[:run_args])
    end

    def determine_variant_name
      parts = [ @options[ :build_type ].downcase ]
      Options::FLAGS_MAP.each do |flag_name, var_name|
        parts << flag_name if @options[ :cmake_flags ][ var_name ] == 'ON'
      end
      parts.join( '-' )
    end

    def determine_binary_path
      postfix = @options[:build_type] == 'Debug' ? '_d' : ''
      File.join( @build_dir, 'bin', "Volt#{postfix}" )
    end

    def configure_required?
      return true unless File.exist?( File.join( @build_dir, 'CMakeCache.txt' ) )
      return true unless File.exist?( @cache.path )

      cached_state = JSON.parse( File.read( @cache.path ) ) rescue nil
      return true unless cached_state

      cached_state['build_type'] != @options[:build_type] or cached_state['cmake_flags'] != @options[:cmake_flags]
    end
  end


end
