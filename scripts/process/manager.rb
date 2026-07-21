# frozen_string_literal: true


module Volt


  module ProcessManager

    class << self

      def fork_child( &block )
        pid = Process.fork do
          Termination.reset_traps!
          yield
        end
        Termination.register( pid )
        pid
      end

      def wait_any( flags = 0 )
        pid, status = Process.wait2( -1, flags )
        if pid
          Termination.unregister( pid )
          [ pid, status ]
        else
          nil
        end
      rescue Errno::ECHILD
        nil
      end

    end

  end


end
