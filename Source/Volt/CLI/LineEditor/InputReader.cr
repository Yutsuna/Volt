require "./InputEvent"


module Volt::CLI


  module InputReader


    extend self

    @@pending = Deque(UInt8).new

    def read_event( io : IO = STDIN ) : InputEvent
      fill_queue( io ) if @@pending.empty?
      return InputEvent.new( KeyEvent::CtrlD ) if @@pending.empty?

      byte = @@pending.shift

      case byte
      when 1      then InputEvent.new( KeyEvent::CtrlA )
      when 3      then InputEvent.new( KeyEvent::CtrlC )
      when 4      then InputEvent.new( KeyEvent::CtrlD )
      when 5      then InputEvent.new( KeyEvent::CtrlE )
      when 9      then InputEvent.new( KeyEvent::Tab )
      when 10, 13 then InputEvent.new( KeyEvent::Enter )
      when 127    then InputEvent.new( KeyEvent::Backspace )
      when 27     then handle_escape_sequence( io )
      else             decode_text( byte, io )
      end
    end

    def reset : Nil
      @@pending.clear
    end

    #------------------------------------------------------------------------------------

    private def fill_queue( io : IO ) : Nil
      slot = Bytes.new( 256 )
      bytes_read = io.read( slot )
      bytes_read.times { |i| @@pending << slot[ i ] }
    end

    # Pops the next byte, doing a short timed read when the queue is empty so that a
    # lone ESC keypress is distinguishable from the start of an escape sequence.
    private def next_pending_byte?( io : IO ) : UInt8?
      return @@pending.shift unless @@pending.empty?

      if io.is_a?( IO::FileDescriptor )
        previous_timeout = io.read_timeout
        io.read_timeout = 20.milliseconds
        begin
          fill_queue( io )
        rescue IO::TimeoutError
        ensure
          io.read_timeout = previous_timeout
        end
      else
        fill_queue( io )
      end

      @@pending.empty? ? nil : @@pending.shift
    end

    #------------------------------------------------------------------------------------

    private def handle_escape_sequence( io : IO ) : InputEvent
      first = next_pending_byte?( io )
      return InputEvent.new( KeyEvent::Ignored ) if first.nil?

      case first
      when '['.ord then parse_csi( io )
      when 'O'.ord then parse_ss3( io )
      else              InputEvent.new( KeyEvent::Ignored )
      end
    end

    private def parse_csi( io : IO ) : InputEvent
      seq = String.build do |builder|
        loop do
          byte = next_pending_byte?( io )
          return InputEvent.new( KeyEvent::Ignored ) if byte.nil?
          builder << byte.chr
          break if 0x40 <= byte <= 0x7E
        end
      end

      case seq
      when "A"          then InputEvent.new( KeyEvent::ArrowUp )
      when "B"          then InputEvent.new( KeyEvent::ArrowDown )
      when "C"          then InputEvent.new( KeyEvent::ArrowRight )
      when "D"          then InputEvent.new( KeyEvent::ArrowLeft )
      when "H", "1~", "7~" then InputEvent.new( KeyEvent::Home )
      when "F", "4~", "8~" then InputEvent.new( KeyEvent::End )
      when "3~"         then InputEvent.new( KeyEvent::Delete )
      when "1;5D"       then InputEvent.new( KeyEvent::CtrlLeft )
      when "1;5C"       then InputEvent.new( KeyEvent::CtrlRight )
      else                   InputEvent.new( KeyEvent::Ignored )
      end
    end

    private def parse_ss3( io : IO ) : InputEvent
      byte = next_pending_byte?( io )
      return InputEvent.new( KeyEvent::Ignored ) if byte.nil?

      case byte.chr
      when 'A' then InputEvent.new( KeyEvent::ArrowUp )
      when 'B' then InputEvent.new( KeyEvent::ArrowDown )
      when 'C' then InputEvent.new( KeyEvent::ArrowRight )
      when 'D' then InputEvent.new( KeyEvent::ArrowLeft )
      when 'H' then InputEvent.new( KeyEvent::Home )
      when 'F' then InputEvent.new( KeyEvent::End )
      else          InputEvent.new( KeyEvent::Ignored )
      end
    end

    #------------------------------------------------------------------------------------

    private def decode_text( byte : UInt8, io : IO ) : InputEvent
      char = decode_utf8( byte, io )
      return InputEvent.new( KeyEvent::Ignored ) if char.nil?

      # A single keystroke yields one char; a paste leaves more printable bytes queued.
      unless !@@pending.empty? && printable_lead?( @@pending.first )
        return InputEvent.new( KeyEvent::Char, char )
      end

      text = String.build do |builder|
        builder << char
        while !@@pending.empty? && printable_lead?( @@pending.first )
          next_char = decode_utf8( @@pending.shift, io )
          builder << next_char if next_char
        end
      end

      InputEvent.new( KeyEvent::Text, text: text )
    end

    private def printable_lead?( byte : UInt8 ) : Bool
      byte >= 32 && byte != 127
    end

    private def decode_utf8( lead : UInt8, io : IO ) : Char?
      length = utf8_length( lead )
      return nil if length == 0
      return lead.chr if length == 1

      bytes = Bytes.new( length )
      bytes[ 0 ] = lead
      ( 1...length ).each do |i|
        continuation = next_pending_byte?( io )
        return nil if continuation.nil?
        bytes[ i ] = continuation
      end

      String.new( bytes )[ 0 ]?
    end

    private def utf8_length( lead : UInt8 ) : Int32
      return 1 if lead & 0x80 == 0
      return 2 if lead & 0xE0 == 0xC0
      return 3 if lead & 0xF0 == 0xE0
      return 4 if lead & 0xF8 == 0xF0
      0
    end


  end


end
