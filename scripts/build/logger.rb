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
      def info( message, prefix: nil )
        tag = prefix ? "#{AnsiColor::GREY}[Volt:#{prefix}]#{AnsiColor::RESET} " : ""
        puts "#{tag}#{AnsiColor::GREY}#{message}#{AnsiColor::RESET}"
      end

      def ok( message, prefix: nil )
        log message, " OK ", AnsiColor::GREEN, prefix: prefix
      end

      def warn( message, prefix: nil )
        log message, "WARN", AnsiColor::YELLOW, prefix: prefix
      end

      def fatal!( message, prefix: nil )
        log message, " ERR ", AnsiColor::RED, prefix: prefix
        exit 84
      end

      private

      def log( message, label, color = AnsiColor::CYAN, prefix: nil )
        variant_tag = prefix ? "#{AnsiColor::GREY}[Volt:#{prefix}]#{AnsiColor::RESET} " : ""
        status_tag = "#{color}#{AnsiColor::BOLD}[#{label}]#{AnsiColor::RESET}"
        puts "#{variant_tag}#{status_tag} #{message}"
      end
    end

  end


end
