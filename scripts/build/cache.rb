# frozen_string_literal: true

require 'json'


module Volt::Build


  class Cache

    PATH          = 'build/.volt_build_cache.json'
    GLOB_PATTERN  = '{CMakeLists.txt,VERSION.md,flake.nix,cmake/**/*,nix/**/*,source/Volt/**/*}'

    def initialize( build_type, cmake_flags, binary_path )
      @build_type   = build_type
      @cmake_flags  = cmake_flags
      @binary_path  = binary_path
    end

    def valid?
      return false unless File.exist?( PATH ) and File.exist?( @binary_path )
      cached_state = JSON.parse( File.read( PATH ), symbolize_names: true ) rescue nil
      return false unless cached_state
      return false if cached_state[ :build_type ] != @build_type
      return false if cached_state[ :cmake_flags ] != @cmake_flags
      cached_state[ :files ] == current_state[ :files ]
    end

    def save!
      File.write( PATH, JSON.pretty_generate( current_state ) )
    end

    private

    def current_state
      @current_state ||= {
        build_type:   @build_type,
        cmake_flags:  @cmake_flags,
        files:        compute_files_metadata
      }
    end

    def compute_files_metadata
      metadata = {}
      trackable_files.each do |file|
        stat = File.stat( file )
        metadata[ file ] = { mtime: stat.mtime.to_i, size: stat.size }
      rescue Errno::ENOENT
      end
      metadata
    end

    def trackable_files
      Dir.glob( GLOB_PATTERN )
        .select { |f| File.file?(f) }
        .reject { |f| f.start_with?('build/') || f.start_with?('.git/') }
        .sort
    end

  end


end
