# frozen_string_literal: true

require 'fileutils'
require 'etc'


module Volt
  module Build


    module SystemInfo
      class << self
        def total_cpus
          @total_cpus ||= Etc.nprocessors
        end

        def total_memory_gb
          @total_memory_gb ||= fetch_system_memory_gb
        end

        def max_safe_parallel_jobs
          mem_jobs = ( total_memory_gb / 3.0 ).floor
          cpu_jobs = ( total_cpus / 2.0 ).ceil
          [ mem_jobs, cpu_jobs ].min.clamp( 1, total_cpus )
        end

        def ninja_threads_per_job( active_jobs_count )
          count = [ active_jobs_count, 1 ].max
          [ ( total_cpus.to_f / count ).floor, 1 ].max
        end

        private

        def fetch_system_memory_gb
          if File.exist?( '/proc/meminfo' )
            mem_line = File.readlines( '/proc/meminfo' ).find { |l| l.start_with?( 'MemTotal:' ) }
            if mem_line && ( m = mem_line.match( /(\d+)/ ) )
              return m[ 1 ].to_i / 1_048_576.0
            end
          end

          pages = Etc.sysconf( Etc::SC_PHYS_PAGES ) rescue 2_097_152
          page_size = Etc.sysconf( Etc::SC_PAGE_SIZE ) rescue 4096
          ( pages * page_size ) / ( 1024.0**3 )
        rescue
          8.0
        end
      end
    end


  end
end
