# frozen_string_literal: true

require 'set'
require 'timeout'


module Volt


  module ProcessManager


    module Termination

      @monitored_pids = Set.new
      @mutex          = Mutex.new
      @interrupted    = false
      @original_traps = {}

      class << self

        def setup!
          %w[INT TERM HUP QUIT].each do |sig|
            @original_traps[ sig ] = Signal.trap( sig ) { Thread.new { handle_signal( sig ) } }
          end
        end

        def reset_traps!
          @original_traps.each { |sig, handler| Signal.trap( sig, handler || 'DEFAULT' ) }
          @mutex.synchronize { @monitored_pids.clear }
        end

        def register( pid )
          @mutex.synchronize { @monitored_pids << pid }
        end

        def unregister( pid )
          @mutex.synchronize { @monitored_pids.delete( pid ) }
        end

        def terminate_all!
          pids = @mutex.synchronize { @monitored_pids.to_a }
          return if pids.empty?

          pids.each { |pid| Process.kill( 'TERM', pid ) rescue nil }

          begin
            Timeout.timeout( 3 ) do
              pids.each { |pid| Process.waitpid( pid ) rescue nil }
            end
          rescue Timeout::Error
            pids.each do |pid|
              Process.kill( 'KILL', pid ) rescue nil
              Process.waitpid( pid ) rescue nil
            end
          end

          @mutex.synchronize { @monitored_pids.clear }
        end

        private

        def handle_signal( sig )
          @mutex.synchronize do
            return if @interrupted
            @interrupted = true
          end

          puts "\n" if sig == 'INT'
          $stderr.puts "Received signal #{sig}, terminating all child processes..."

          terminate_all!

          exit_code = case sig
                      when 'INT'  then 130
                      when 'TERM' then 143
                      else 1
                      end
          exit exit_code
        end

      end

    end


  end


end
