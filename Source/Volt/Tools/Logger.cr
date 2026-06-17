module Volt


  module EAnsiColor
    RED     = "\e[31m"
    GREEN   = "\e[32m"
    YELLOW  = "\e[33m"
    CYAN    = "\e[36m"
    BLUE    = "\e[34m"
    GREY    = "\e[90m"
    BOLD    = "\e[1m"
    RESET   = "\e[0m"
  end


  module FLog

    extend self

    #--------------------------------------------------------------------------

    def log ( message : String, prefix : String = "Krystal", color : String = EAnsiColor::CYAN)
      tag = "#{color}#{EAnsiColor::BOLD}[#{prefix}]#{EAnsiColor::RESET}"
      STDOUT.puts "#{tag} #{message}"
      STDOUT.flush
    end

    #--------------------------------------------------------------------------

    def ok ( message    : String )
      log message, "  OK  ",    EAnsiColor::GREEN
    end

    def warn ( message  : String )
      log message, "  WARN  ",  EAnsiColor::YELLOW
    end

    def error ( message : String )
      log message, "  ERR  ",   EAnsiColor::RED
    end

    def step ( message  : String )
      log message, "  >>  ",    EAnsiColor::BLUE
    end

    def info ( message  : String )
      STDOUT.puts( "#{EAnsiColor::GREY}#{message}#{EAnsiColor::RESET}" )
      STDOUT.flush
    end

    def command ( message : String )
      STDOUT.print( "\r#{EAnsiColor::GREY}#{message}#{EAnsiColor::RESET}" )
      STDOUT.flush
    end

    #--------------------------------------------------------------------------

  end


end
