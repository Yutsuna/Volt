require "./InputEvent"


module Volt::CLI


  module InputReader


    extend self

    def read_char : InputEvent
      buffer = Bytes.new(8)
      bytes_read = STDIN.read(buffer)
      return InputEvent.new(KeyEvent::Enter) if bytes_read == 0

      byte = buffer[0]

      case byte
      when 1      then return InputEvent.new( KeyEvent::CtrlA )
      when 3      then return InputEvent.new( KeyEvent::CtrlC )
      when 4      then return InputEvent.new( KeyEvent::CtrlD )
      when 10, 13 then return InputEvent.new( KeyEvent::Enter )
      when 127    then return InputEvent.new( KeyEvent::Backspace )
      when 27     then return handle_escape_sequence( buffer, bytes_read )
      else        return InputEvent.new( KeyEvent::Char, byte.chr )
      end
    end

    private def handle_escape_sequence(buffer : Bytes, bytes_read : Int32) : InputEvent
      return InputEvent.new(KeyEvent::Char, 27.chr) unless bytes_read > 1 && buffer[1] == '['.ord

      seq = String.new(buffer[2, bytes_read - 2])
      case seq
      when "A"    then InputEvent.new( KeyEvent::ArrowUp )
      when "B"    then InputEvent.new( KeyEvent::ArrowDown )
      when "C"    then InputEvent.new( KeyEvent::ArrowRight )
      when "D"    then InputEvent.new( KeyEvent::ArrowLeft )
      when "1;5D" then InputEvent.new( KeyEvent::CtrlLeft )
      when "1;5C" then InputEvent.new( KeyEvent::CtrlRight )
      else InputEvent.new(KeyEvent::Char, 27.chr)
      end
    end


  end


end
