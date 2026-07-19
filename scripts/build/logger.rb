# frozen_string_literal: true

module Volt::Build


  module Logger


    module AnsiColor
      RED     = "\e[31m"
      GREEN   = "\e[32m"
      YELLOW  = "\e[33m"
      CYAN    = "\e[36m"
      BLUE    = "\e[34m"
      GREY    = "\e[90m"
      BOLD    = "\e[1m"
      RESET   = "\e[0m"
    end

    class << self
      def info( message )
        puts "#{AnsiColor::GREY}#{message}#{AnsiColor::RESET}"
      end

      def ok( message )
        log message, " OK ", AnsiColor::GREEN
      end

      def fatal!( message )
        log message, " ERR ", AnsiColor::RED
        exit 84
      end

      private

      def log( message, prefix, color = AnsiColor::CYAN )
        tag = "#{color}#{AnsiColor::BOLD}[#{prefix}]#{AnsiColor::RESET}"
        puts "#{tag} #{message}"
      end
    end

  end


end
